// "Translucency" feature: refraction sources, back-face depth, refractive objects, transparent objects (Weighted
// Blended OIT or sorted), hashed alpha toggle for the depth prepass.
#include "translucency_internal.hpp"

#include <oxwald/core/cvar.hpp>
#include <oxwald/core/profile.hpp>
#include <oxwald/render/features/raytracing/raytracing.hpp>

namespace ox::render {

namespace {

using S = Scalability;

CVar<int> cvMethod("r.Translucency.Method", 0, "Transparent objects: Auto (OIT unless the scene is simple), OIT, Sorted",
                   CVarEnum{"Auto", "OIT", "Sorted"});
CVar<int> cvSortedMax("r.Translucency.SortedMaxInstances", 4,
                      "Auto method: visible transparent instances up to which the sorted path is used", 0, 4096);
CVar<bool> cvRefraction("r.Refraction", true, "Screen-space refraction of refractive materials (else drawn as glass "
                        "over the unrefracted scene)");
CVar<int> cvRefractionMips("r.Refraction.Mips", 6, "Blur levels of the refraction source (rough refraction)",
                           S::Reflections, {3, 5, 6, 7});
CVar<bool> cvBackface("r.Refraction.BackfaceDepth", true,
                      "Back-face depth prepass for refractive objects (thickness for absorption / refraction)",
                      S::Shading, {false, true, true, true});
CVar<float> cvRefractionStrength("r.Refraction.Strength", 1.0f, "Scale of the refraction offset", 0.0f, 4.0f);
CVar<float> cvRefractionDistance("r.Refraction.MaxDistance", 4.0f,
                                 "Background distance (m) considered behind a refractive surface", 0.0f, 100.0f);
CVar<int> cvAlphaDither("r.AlphaTest.Dither", 2,
                        "Hashed (stochastic) alpha test in the depth prepass: Off, On, Auto (on with TAA)",
                        CVarEnum{"Off", "On", "Auto"});

struct TranslucentPush {
    u64 view = 0, scene = 0, drawIds = 0;
    u32 refraction = kInvalidIndex;
    u32 refractionMips = 1;
    u32 sceneDepth = kInvalidIndex;
    u32 backDepth = kInvalidIndex;
    u32 fog = kInvalidIndex;
    u32 flags = 0;
    f32 refractionStrength = 1.0f;
    f32 maxRefractionDistance = 4.0f;
};
static_assert(sizeof(TranslucentPush) == 56);

// Copy of a list keeping the batches accepted by `keep` (instance ids stay valid: batches index the same buffer).
template <class F>
DrawList filtered(const DrawList& src, F&& keep) {
    DrawList out;
    out.instanceIds = src.instanceIds;
    for (const DrawBatch& b : src.batches) {
        if (!keep(b)) continue;
        out.batches.push_back(b);
        out.instanceCount += b.instanceCount;
        out.triangleCount += u64(b.indexCount / 3) * b.instanceCount;
    }
    return out;
}

class TranslucencyFeature final : public IRenderFeature {
public:
    std::string_view name() const override { return "Translucency"; }
    InjectionMask injectionPoints() const override {
        return maskOf(InjectionPoint::AfterOpaque, InjectionPoint::Translucency);
    }
    i32 order() const override { return 0; }
    std::vector<std::string_view> provides() const override {
        return {res::kSceneColorRefraction, res::kSceneDepthCopy, res::kRefractionBackDepth};
    }
    std::vector<std::string> cvarNames() const override {
        return {"r.Translucency.Method", "r.Translucency.SortedMaxInstances", "r.Refraction", "r.Refraction.Mips",
                "r.Refraction.BackfaceDepth", "r.Refraction.Strength", "r.Refraction.MaxDistance", "r.AlphaTest.Dither"};
    }

    bool initialize(FeatureInitContext& ctx) override {
        rhi::Device& dev = ctx.device;
        const std::vector<rhi::ShaderDefine> fog = translucency::contractDefines();
        auto mesh = [&](const char* name, u32 variant, const char* mode, std::vector<VkFormat> colors,
                        std::vector<rhi::BlendState> blend) {
            rhi::GraphicsPipelineDesc d;
            d.name = std::format("translucency.{}.v{}", name, variant);
            d.vertex = rhi::ShaderStageDesc::file("render/translucency/mesh.vert");
            std::vector<rhi::ShaderDefine> defs = fog;
            defs.push_back({mode});
            d.fragment = rhi::ShaderStageDesc::file("render/translucency/translucent.frag", defs);
            d.colorFormats = std::move(colors);
            d.blend = std::move(blend);
            d.depthFormat = formats::kDepth;
            d.depth = {true, false, VK_COMPARE_OP_GREATER_OR_EQUAL};
            d.raster.cullMode = (variant & kVariantDoubleSided) ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
            d.raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            return dev.createGraphicsPipeline(d);
        };
        rhi::BlendState reveal;
        reveal.enable = true;
        reveal.srcColor = reveal.srcAlpha = VK_BLEND_FACTOR_ZERO;
        reveal.dstColor = reveal.dstAlpha = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        for (u32 v = 0; v < kVariantCount; ++v) {
            m_refractive[v] = mesh("refractive", v, "OX_MODE_REFRACTIVE", {formats::kSceneColor}, {rhi::BlendState::opaque()});
            m_oit[v] = mesh("oit", v, "OX_MODE_OIT", {formats::kSceneColor, VK_FORMAT_R8_UNORM},
                            {rhi::BlendState::additive(), reveal});
            m_sorted[v] = mesh("sorted", v, "OX_MODE_SORTED", {formats::kSceneColor}, {rhi::BlendState::premultiplied()});
        }
        rhi::GraphicsPipelineDesc b;
        b.name = "translucency.backfaceDepth";
        b.vertex = rhi::ShaderStageDesc::file("render/translucency/mesh.vert");
        b.depthFormat = formats::kDepth;
        b.depth = {true, true, VK_COMPARE_OP_GREATER_OR_EQUAL};
        b.raster.cullMode = VK_CULL_MODE_FRONT_BIT;
        b.raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        m_backface = dev.createGraphicsPipeline(b);
        rhi::BlendState over = rhi::BlendState::alpha();
        m_composite = createFullscreenPipeline(dev, "translucency.oitComposite", "render/translucency/oit_composite.frag",
                                               {formats::kSceneColor}, {over});
        m_source = translucency::createSourcePipeline(dev);
        return true;
    }

    void shutdown(rhi::Device& dev) override {
        for (u32 v = 0; v < kVariantCount; ++v) {
            dev.destroy(m_refractive[v]);
            dev.destroy(m_oit[v]);
            dev.destroy(m_sorted[v]);
        }
        dev.destroy(m_backface);
        dev.destroy(m_composite);
        dev.destroy(m_source);
    }

    void setup(FeatureContext& ctx) override {
        if (ctx.point() == InjectionPoint::AfterOpaque) setupAfterOpaque(ctx);
        else setupTranslucency(ctx);
    }

private:
    void setupAfterOpaque(FeatureContext& ctx) {
        OX_PROFILE_ZONE();
        const i32 dither = cvAlphaDither;
        if (dither == 1 || (dither == 2 && ctx.settings().antiAliasing == 2)) ctx.viewConstants().flags |= kViewFlagHashedAlpha;

        // Declared every frame; the graph culls the copies when nothing samples them.
        translucency::ensureSources(ctx, m_source);

        const DrawList& refractive = ctx.drawLists()[DrawBucket::Refractive];
        if (refractive.empty() || !cvBackface || !cvRefraction) return;
        if (rt::rayTracedRefractionActive(ctx.settings(), ctx.caps())) return; // TranslucencyRT owns refraction
        // Closed single-sided meshes only: double-sided materials keep the material thickness.
        DrawList closed = filtered(refractive, [](const DrawBatch& b) { return !(b.variant & kVariantDoubleSided); });
        if (closed.empty()) return;
        const rhi::RGTexture back = ctx.graph().createTexture(
            translucency::textureDesc(formats::kDepth, ctx.renderExtent(), "RefractionBackDepth"));
        FrameState* fs = &frameState(ctx);
        const VkDeviceAddress view = ctx.viewAddress(), scene = ctx.sceneAddress();
        IRenderFeature* self = this;
        ctx.graph()
            .addPass("Translucency.BackfaceDepth")
            .depth(back, VK_ATTACHMENT_LOAD_OP_CLEAR, {0.0f, 0})
            .execute([=, this, list = std::move(closed)](rhi::PassContext& p) {
                FeatureContext fc(*fs, self, InjectionPoint::AfterOpaque);
                TranslucentPush pc;
                pc.view = view, pc.scene = scene, pc.drawIds = list.instanceIds;
                const rhi::PipelineHandle pipes[1] = {m_backface};
                fc.drawBatches(p.cmd, list, pipes, &pc, sizeof(pc));
            });
        ctx.resources().setTexture(res::kRefractionBackDepth, back);
    }

    void setupTranslucency(FeatureContext& ctx) {
        OX_PROFILE_ZONE();
        FrameResources& R = ctx.resources();
        // With ray traced refraction active ("TranslucencyRT", raytracing area) refractive materials are rewritten in
        // SceneColorHDR by the RT pass: the raster path skips them.
        static const DrawList kEmpty;
        const bool rtRefraction = rt::rayTracedRefractionActive(ctx.settings(), ctx.caps());
        const DrawList& refractiveList = rtRefraction ? kEmpty : ctx.drawLists()[DrawBucket::Refractive];
        const DrawList& transparentList = ctx.drawLists()[DrawBucket::Transparent];
        if (refractiveList.empty() && transparentList.empty()) return;

        rhi::RenderGraph& g = ctx.graph();
        const rhi::RGTexture hdr = R.texture(res::kSceneColorHDR), depth = R.texture(res::kDepth);
        const VkDeviceAddress view = ctx.viewAddress(), scene = ctx.sceneAddress();
        FrameState* fs = &frameState(ctx);
        IRenderFeature* self = this;
        const Extent2D e = ctx.renderExtent();

        // --- refractive objects (back to front, blend off) ---
        if (!refractiveList.empty()) {
            const translucency::RefractionSources src = translucency::ensureSources(ctx, m_source);
            const rhi::RGTexture back = R.texture(res::kRefractionBackDepth);
            const bool refraction = cvRefraction;
            rhi::PassBuilder pb = g.addPass("Translucency.Refractive");
            pb.color(hdr, VK_ATTACHMENT_LOAD_OP_LOAD).depth(depth, VK_ATTACHMENT_LOAD_OP_LOAD, {}, true);
            pb.read(src.color, rhi::Access::SampledFragment).read(src.depth, rhi::Access::SampledFragment);
            if (back.valid()) pb.read(back, rhi::Access::SampledFragment);
            const rhi::RGTexture fog = translucency::declareLightingReads(pb, R);
            const u32 mips = src.mips;
            const f32 strength = refraction ? f32(cvRefractionStrength) : 0.0f;
            const f32 maxDist = cvRefractionDistance;
            pb.execute([=, this, &refractiveList](rhi::PassContext& p) {
                FeatureContext fc(*fs, self, InjectionPoint::Translucency);
                TranslucentPush pc;
                pc.view = view, pc.scene = scene, pc.drawIds = refractiveList.instanceIds;
                pc.refraction = p.sampledIndex(src.color);
                pc.refractionMips = mips;
                pc.sceneDepth = p.sampledIndex(src.depth);
                pc.backDepth = back.valid() ? p.sampledIndex(back) : kInvalidIndex;
                pc.fog = fog.valid() ? p.sampledIndex(fog) : kInvalidIndex;
                pc.flags = back.valid() ? 1u : 0u;
                pc.refractionStrength = strength;
                pc.maxRefractionDistance = maxDist;
                fc.drawBatches(p.cmd, refractiveList, m_refractive, &pc, sizeof(pc));
            });
        }

        if (transparentList.empty()) return;

        // --- transparent: split into the OIT and the sorted path ---
        const i32 method = cvMethod;
        const bool simple = transparentList.instanceCount <= u32(std::max(i32(cvSortedMax), 0));
        const bool allSorted = method == 2 || (method == 0 && simple);
        GpuScene& gs = ctx.scene();
        auto isSorted = [&](const DrawBatch& b) {
            return allSorted || (gs.material(b.materialIndex).flags & kMaterialSortedTranslucency) != 0;
        };
        DrawList oit = filtered(transparentList, [&](const DrawBatch& b) { return !isSorted(b); });
        DrawList sorted = filtered(transparentList, [&](const DrawBatch& b) { return isSorted(b); });

        if (!oit.empty()) {
            const rhi::RGTexture accum = g.createTexture(translucency::textureDesc(formats::kSceneColor, e, "OIT.Accum"));
            const rhi::RGTexture reveal = g.createTexture(translucency::textureDesc(VK_FORMAT_R8_UNORM, e, "OIT.Revealage"));
            rhi::PassBuilder pb = g.addPass("Translucency.OIT");
            pb.color(accum, VK_ATTACHMENT_LOAD_OP_CLEAR, rhi::ClearColor::rgba(0, 0, 0, 0))
                .color(reveal, VK_ATTACHMENT_LOAD_OP_CLEAR, rhi::ClearColor::rgba(1, 1, 1, 1))
                .depth(depth, VK_ATTACHMENT_LOAD_OP_LOAD, {}, true);
            const rhi::RGTexture fog = translucency::declareLightingReads(pb, R);
            pb.execute([=, this, list = std::move(oit)](rhi::PassContext& p) {
                FeatureContext fc(*fs, self, InjectionPoint::Translucency);
                TranslucentPush pc;
                pc.view = view, pc.scene = scene, pc.drawIds = list.instanceIds;
                pc.fog = fog.valid() ? p.sampledIndex(fog) : kInvalidIndex;
                fc.drawBatches(p.cmd, list, m_oit, &pc, sizeof(pc));
            });
            g.addPass("Translucency.OITComposite")
                .read(accum, rhi::Access::SampledFragment)
                .read(reveal, rhi::Access::SampledFragment)
                .color(hdr, VK_ATTACHMENT_LOAD_OP_LOAD)
                .execute([=, this](rhi::PassContext& p) {
                    const u32 pc[2] = {p.sampledIndex(accum), p.sampledIndex(reveal)};
                    drawFullscreen(p.cmd, m_composite, pc, sizeof(pc));
                });
        }
        if (!sorted.empty()) {
            rhi::PassBuilder pb = g.addPass("Translucency.Sorted");
            pb.color(hdr, VK_ATTACHMENT_LOAD_OP_LOAD).depth(depth, VK_ATTACHMENT_LOAD_OP_LOAD, {}, true);
            const rhi::RGTexture fog = translucency::declareLightingReads(pb, R);
            pb.execute([=, this, list = std::move(sorted)](rhi::PassContext& p) {
                FeatureContext fc(*fs, self, InjectionPoint::Translucency);
                TranslucentPush pc;
                pc.view = view, pc.scene = scene, pc.drawIds = list.instanceIds;
                pc.fog = fog.valid() ? p.sampledIndex(fog) : kInvalidIndex;
                fc.drawBatches(p.cmd, list, m_sorted, &pc, sizeof(pc));
            });
        }
    }

    rhi::PipelineHandle m_refractive[kVariantCount];
    rhi::PipelineHandle m_oit[kVariantCount];
    rhi::PipelineHandle m_sorted[kVariantCount];
    rhi::PipelineHandle m_backface;
    rhi::PipelineHandle m_composite;
    rhi::PipelineHandle m_source;
};

} // namespace

namespace translucency {
bool refractionEnabled() { return cvRefraction; }
i32 refractionMipCount() { return cvRefractionMips; }
} // namespace translucency

std::unique_ptr<IRenderFeature> makeTranslucencyFeature() { return std::make_unique<TranslucencyFeature>(); }

} // namespace ox::render
