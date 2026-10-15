// Variable rate shading (VK_KHR_fragment_shading_rate): content and motion adaptive shading rate image.
//
//   AfterDepth        VRS.Generate   last frame's tile statistics (reprojected with Velocity) + motion + depth of
//                                    field + periphery + hysteresis → R8_UINT rate image (one texel per 16×16 px on
//                                    NVIDIA), published as "ShadingRate"
//   AfterOpaque       VRS.AnalyzeOpaque  statistics of the opaque SceneColorHDR (transient)
//   BeforePostProcess (attach)       the rate image is attached to the declared colour passes of r.VRS.Passes
//                     VRS.Analyze    statistics of this frame's SceneColorHDR (before TAA / upscaling), combined
//                                    with the opaque statistics (larger relative error per axis)
//   Debug             VRS.Overlay    colour overlay of the rate image (r.VRS.Mode DebugOverlay or r.VRS.Overlay)
#include "vrs_internal.hpp"

#include "../postprocess/pp_common.hpp"

#include <oxwald/render/render.hpp>
#include <oxwald/rhi/device.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <unordered_map>

namespace ox::render {
namespace vrs {

namespace {

using S = Scalability;

// Values chosen from the measurements in docs/dev/perf.md ("Variable rate shading", RTX 3080): on only at Low and
// Medium, where the gain matters more than the last bit of texture detail; High / Ultra keep full-rate shading (the
// gain there was 0–6 %). The code default is off like the other optional effects, so a bare Renderer (tests, tools)
// renders exact images; the threshold / max rate defaults are the High values, used if r.VRS is switched on by hand.
CVar<bool> cvVrs("r.VRS", false, "Variable rate shading (VK_KHR_fragment_shading_rate; no-op where unsupported)",
                 S::Shading, {true, true, false, false});
CVar<int> cvMode("r.VRS.Mode", 0, "Shading rate source: Adaptive (content + motion), Motion, Foveated, DebugOverlay",
                 CVarEnum{"Adaptive", "Motion", "Foveated", "DebugOverlay"});
CVar<float> cvThreshold("r.VRS.Threshold", 0.02f,
                        "Adaptive VRS just-noticeable difference (relative luma gradient); higher = coarser",
                        S::Shading, {0.05f, 0.03f, 0.02f, 0.02f});
CVar<int> cvMaxRate("r.VRS.MaxRate", 2, "Coarsest fragment size (2 = up to 2x2, 4 = up to 4x4)", S::Shading,
                    {4, 2, 2, 2});
CVar<float> cvHysteresis("r.VRS.Hysteresis", 0.25f,
                         "Threshold band: coarsen below Threshold*(1-h), refine above Threshold*(1+h)", 0.0f, 0.9f);
CVar<float> cvMotionScale("r.VRS.MotionScale", 0.15f,
                          "Adaptive VRS: error divided by (1 + scale * pixels moved per frame)", 0.0f, 10.0f);
CVar<float> cvMotionPixels("r.VRS.MotionPixels", 6.0f,
                           "Motion VRS: 2x from this many pixels per frame, 4x from three times as many", 0.0f, 1000.0f);
CVar<float> cvPeriphery("r.VRS.Periphery", 0.0f,
                        "Coarser shading beyond this normalised distance from the centre (0 = off; Foveated: 0.5)",
                        0.0f, 1.0f);
CVar<int> cvRefresh("r.VRS.RefreshPeriod", 8,
                    "Adaptive VRS: every N frames each tile is shaded at full rate once to re-measure its detail (0 = off)",
                    0, 1000);
CVar<bool> cvDof("r.VRS.DepthOfField", true, "Coarser shading where depth of field blurs (CoC >= 3 px)");
CVar<bool> cvOverlay("r.VRS.Overlay", false, "Draw the shading rate image over the frame");
CVar<std::string> cvPasses("r.VRS.Passes", std::string(),
                           "Comma-separated pass names that get the rate image (empty = built-in list)");

constexpr i32 kAfterDepthOrder = 1000;
constexpr i32 kBeforePostOrder = -100000; // before TAA / TAAU / DLSS (order 0) and auto exposure (-100)

struct ViewState final : IFeatureViewState {
    rhi::RGTexture rate;     // this frame's rate image (graph handle, valid during the frame's setup)
    rhi::RGTexture stats;    // this frame's statistics (written by Analyze)
    rhi::RGTexture opaqueStats; // statistics of the opaque image (AnalyzeOpaque), transient
    bool rateValid = false;
    bool opaqueValid = false;
    u32 tileSize = 0;
    Extent2D render{};
    u64 frame = ~0ull;
    u32 attached = 0; // passes that got the rate image this frame
};

class VrsFeature final : public IRenderFeature {
public:
    std::string_view name() const override { return "VariableRateShading"; }
    InjectionMask injectionPoints() const override {
        return maskOf(InjectionPoint::AfterDepth, InjectionPoint::AfterOpaque, InjectionPoint::BeforePostProcess,
                      InjectionPoint::Debug);
    }
    i32 order() const override { return kAfterDepthOrder; }
    std::vector<std::string_view> provides() const override { return {kShadingRateResource}; }
    std::vector<std::string> cvarNames() const override {
        return {"r.VRS",           "r.VRS.Mode",          "r.VRS.Threshold",  "r.VRS.MaxRate",
                "r.VRS.Hysteresis", "r.VRS.RefreshPeriod", "r.VRS.MotionScale", "r.VRS.MotionPixels",
                "r.VRS.Periphery",  "r.VRS.DepthOfField",  "r.VRS.Overlay",     "r.VRS.Passes"};
    }
    bool isEnabled(const RenderSettings&, const rhi::DeviceCaps& caps) const override {
        return cvVrs.get() && caps.attachmentShadingRate && chooseTexelSize(caps) != 0 && caps.shadingRateSupported(2, 2);
    }
    bool initialize(FeatureInitContext& ctx) override {
        m_passesCallback = cvPasses.onChanged([this](const std::string&, const std::string&) { m_passesDirty = true; });
        m_passesDirty = true;
        // Without attachment rates isEnabled() is always false: no pipelines to compile (the analysis also needs
        // subgroup arithmetic, which every device with VK_KHR_fragment_shading_rate has).
        if (!availability(ctx.device.caps()).available) return true;
        return m_pipelines.create(ctx.device);
    }
    void shutdown(rhi::Device& d) override {
        if (m_passesCallback) cvPasses.removeCallback(m_passesCallback);
        m_passesCallback = 0;
        m_pipelines.destroy(d);
    }

    void setup(FeatureContext& ctx) override {
        switch (ctx.point()) {
        case InjectionPoint::AfterDepth: generate(ctx); break;
        case InjectionPoint::AfterOpaque: analyzeOpaque(ctx); break;
        case InjectionPoint::BeforePostProcess: attachAndAnalyze(ctx); break;
        case InjectionPoint::Debug: overlay(ctx); break;
        default: break;
        }
    }

private:
    void generate(FeatureContext& ctx) {
        ViewState& vs = ctx.viewState<ViewState>();
        vs.frame = ctx.view().frameIndex();
        vs.rateValid = false;
        vs.opaqueValid = false;
        vs.attached = 0;
        FrameResources& R = ctx.resources();
        const rhi::RGTexture depth = R.texture(res::kDepth), velocity = R.texture(res::kVelocity);
        if (!depth.valid() || !velocity.valid()) return;
        const rhi::DeviceCaps& caps = ctx.caps();
        const u32 tileSize = chooseTexelSize(caps);
        const Extent2D render = ctx.renderExtent();
        const Extent2D tiles = tileCount(render, tileSize);

        rhi::TextureDesc rd;
        rd.format = kRateFormat;
        rd.width = tiles.width;
        rd.height = tiles.height;
        rd.usage = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled | rhi::TextureUsage::ShadingRateAttachment;
        rd.name = "VRS.Rate";
        const HistoryTexture rate = ctx.history("VRS.Rate", rd);
        rhi::TextureDesc sd = rd;
        sd.format = kStatsFormat;
        sd.usage = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled;
        sd.name = "VRS.Stats";
        const HistoryTexture stats = ctx.history("VRS.Stats", sd);

        GenerateInputs in;
        in.view = ctx.viewAddress();
        in.depth = depth;
        in.velocity = velocity;
        in.prevStats = stats.previous;
        in.prevRate = rate.previous;
        in.outRate = rate.current;
        // Statistics exist only if last frame ran the analysis with the same tiling.
        in.historyValid = rate.previousValid && stats.previousValid && m_lastAnalyzed.contains(ctx.view().id()) &&
                          m_lastAnalyzed[ctx.view().id()] + 1 == vs.frame;
        in.params = paramsFromCVars(caps);
        in.renderSize = render;
        in.tileSize = tileSize;
        in.refreshPeriod = u32(std::max(cvRefresh.get(), 0));
        in.frame = u32(vs.frame);
        dofParameters(ctx, in);
        addGeneratePass(ctx.graph(), m_pipelines, in);

        vs.rate = rate.current;
        vs.stats = stats.current;
        vs.rateValid = true;
        vs.tileSize = tileSize;
        vs.render = render;
        R.setTexture(kShadingRateResource, rate.current);
    }

    // The opaque passes of a tile are shaded at the rate the final image of that tile asked for. Behind refractive
    // glass or water the final image shows the opaque surface displaced and magnified, so the final statistics alone
    // would let a detailed floor behind a smooth-looking sphere go coarse (visible blocks in the refraction).
    void analyzeOpaque(FeatureContext& ctx) {
        ViewState& vs = ctx.viewState<ViewState>();
        if (!vs.rateValid || vs.frame != ctx.view().frameIndex()) return;
        const rhi::RGTexture color = ctx.resources().texture(res::kSceneColorHDR);
        if (!color.valid()) return;
        const rhi::TextureDesc& cd = ctx.graph().textureDesc(color);
        if (cd.width != vs.render.width || cd.height != vs.render.height) return;
        const Extent2D tiles = tileCount(vs.render, vs.tileSize);
        rhi::TextureDesc sd;
        sd.format = kStatsFormat;
        sd.width = tiles.width;
        sd.height = tiles.height;
        sd.usage = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled;
        sd.name = "VRS.OpaqueStats";
        AnalyzeInputs in;
        in.color = color;
        in.rate = vs.rate;
        in.rateValid = true;
        in.outStats = ctx.graph().createTexture(sd);
        in.renderSize = vs.render;
        in.tileSize = vs.tileSize;
        addAnalyzePass(ctx.graph(), m_pipelines, in, "VRS.AnalyzeOpaque");
        vs.opaqueStats = in.outStats;
        vs.opaqueValid = true;
    }

    void attachAndAnalyze(FeatureContext& ctx) {
        ViewState& vs = ctx.viewState<ViewState>();
        if (!vs.rateValid || vs.frame != ctx.view().frameIndex()) return;
        // Steady state allocates nothing here: the pass list is parsed again only after r.VRS.Passes changed.
        if (m_passesDirty.exchange(false)) m_passes = parsePassList(cvPasses.get());
        vs.attached = attachToPasses(ctx.graph(), vs.rate, vs.tileSize, vs.render, m_passes);
        const rhi::RGTexture color = ctx.resources().texture(res::kSceneColorHDR);
        if (!color.valid()) return;
        const rhi::TextureDesc& cd = ctx.graph().textureDesc(color);
        if (cd.width != vs.render.width || cd.height != vs.render.height) return; // already upscaled: no statistics
        AnalyzeInputs in;
        in.color = color;
        in.rate = vs.rate;
        in.rateValid = true;
        in.outStats = vs.stats;
        in.opaqueStats = vs.opaqueStats;
        // Only refraction (glass, water) shows the opaque image displaced; elsewhere the final image already shows
        // what the opaque passes produced (fog or particles over it only hide detail), and the unused AnalyzeOpaque
        // pass is culled by the graph.
        in.combineOpaque = vs.opaqueValid && hasRefraction(ctx.graph());
        in.renderSize = vs.render;
        in.tileSize = vs.tileSize;
        addAnalyzePass(ctx.graph(), m_pipelines, in);
        m_lastAnalyzed[ctx.view().id()] = vs.frame;
    }

    void overlay(FeatureContext& ctx) {
        if (!(cvOverlay.get() || cvMode.get() == i32(Mode::DebugOverlay))) return;
        ViewState& vs = ctx.viewState<ViewState>();
        if (!vs.rateValid || vs.frame != ctx.view().frameIndex()) return;
        const rhi::RGTexture ldr = ctx.resources().texture(res::kSceneColorLDR);
        if (!ldr.valid()) return;
        const rhi::RGTexture rate = vs.rate;
        const u32 tileSize = vs.tileSize;
        const glm::vec2 renderSize{f32(vs.render.width), f32(vs.render.height)};
        const rhi::PipelineHandle pipeline = m_pipelines.overlay;
        ctx.graph()
            .addPass("VRS.Overlay")
            .read(rate, rhi::Access::SampledFragment)
            .color(ldr, VK_ATTACHMENT_LOAD_OP_LOAD)
            .execute([=](rhi::PassContext& p) {
                struct {
                    u32 rate, tileSize;
                    glm::vec2 renderSize;
                    f32 opacity;
                } pc{p.sampledIndex(rate), tileSize, renderSize, 0.45f};
                drawFullscreen(p.cmd, pipeline, &pc, sizeof(pc));
            });
    }

    static bool hasRefraction(const rhi::RenderGraph& graph) {
        for (u32 i = 0; i < graph.passCount(); ++i) {
            const std::string& n = graph.passName(i);
            if (n == "Translucency.Refractive" || n == "Water.Surface") return true;
        }
        return false;
    }

    static DecisionParams paramsFromCVars(const rhi::DeviceCaps& caps) {
        DecisionParams p;
        p.mode = Mode(std::clamp(cvMode.get(), 0, 3));
        p.threshold = std::max(cvThreshold.get(), 0.0f);
        p.hysteresis = std::clamp(cvHysteresis.get(), 0.0f, 0.9f);
        p.motionScale = std::max(cvMotionScale.get(), 0.0f);
        p.motionPixels = std::max(cvMotionPixels.get(), 0.0f);
        p.periphery = std::clamp(cvPeriphery.get(), 0.0f, 1.0f);
        p.depthOfField = cvDof.get();
        p.maxLog2 = cvMaxRate.get() >= 4 ? 2u : cvMaxRate.get() >= 2 ? 1u : 0u;
        p.supportedMask = caps.shadingRateMask;
        return p;
    }

    // Mirrors the DepthOfField feature (postprocess/dof.cpp): thin-lens CoC in render pixels.
    static void dofParameters(FeatureContext& ctx, GenerateInputs& in) {
        in.depthOfField = false;
        if (!cvDof.get() || !pp::cvDof.get() || !FeatureRegistry::toggle("DepthOfField").get()) return;
        const PostProcessSettings s = pp::viewSettings(ctx);
        const CameraParams& cam = ctx.view().camera();
        if (s.focusDistance <= 0.0f || cam.projection == CameraParams::Projection::Orthographic) return;
        constexpr f32 kSensorHeightMm = 24.0f;
        const f32 fNumber = s.aperture > 0.0f ? s.aperture : pp::viewAperture(ctx);
        const f32 focalMm = s.focalLength > 0.0f ? s.focalLength : 0.5f * kSensorHeightMm / std::tan(0.5f * cam.verticalFov);
        const f32 f = focalMm * 1e-3f;
        const f32 S = std::max(s.focusDistance, f * 1.01f);
        const f32 apertureDiameter = f / std::max(fNumber, 0.5f);
        const f32 cocScale = 0.5f * apertureDiameter * f / (S - f) / (kSensorHeightMm * 1e-3f) * f32(in.renderSize.height);
        if (cocScale < 0.05f) return;
        in.depthOfField = true;
        in.cocScale = cocScale;
        in.focus = S;
        in.params.depthOfField = true;
    }

    Pipelines m_pipelines;
    std::unordered_map<ViewId, u64> m_lastAnalyzed; // frame index of the last analysis per view
    std::vector<std::string> m_passes;              // r.VRS.Passes, parsed
    std::atomic<bool> m_passesDirty{true};
    usize m_passesCallback = 0;
};

} // namespace

// --- building blocks -----------------------------------------------------------------------------------------

bool Pipelines::create(rhi::Device& device) {
    analyze = createComputePipeline(device, "vrs.analyze", "render/vrs/vrs_analyze.comp");
    generate = createComputePipeline(device, "vrs.generate", "render/vrs/vrs_generate.comp");
    overlay = createFullscreenPipeline(device, "vrs.overlay", "render/vrs/vrs_overlay.frag", {formats::kSceneColorLDR},
                                       {rhi::BlendState::alpha()});
    return analyze && generate && overlay;
}

void Pipelines::destroy(rhi::Device& device) {
    for (rhi::PipelineHandle& p : {std::ref(analyze), std::ref(generate), std::ref(overlay)}) {
        if (p) device.destroy(p);
        p = {};
    }
}

void addGeneratePass(rhi::RenderGraph& graph, const Pipelines& pl, const GenerateInputs& in) {
    const Extent2D tiles = tileCount(in.renderSize, in.tileSize);
    rhi::PassBuilder pb = graph.addPass("VRS.Generate", rhi::PassType::Compute);
    pb.read(in.depth, rhi::Access::SampledCompute).read(in.velocity, rhi::Access::SampledCompute);
    pb.read(in.prevStats, rhi::Access::SampledCompute).read(in.prevRate, rhi::Access::SampledCompute);
    pb.overwrite(in.outRate, rhi::Access::StorageWriteCompute);
    const rhi::PipelineHandle pipeline = pl.generate;
    const GenerateInputs c = in;
    pb.execute([=](rhi::PassContext& p) {
        struct {
            u64 view;
            u32 depth, velocity, prevStats, prevRate, outRate, flags;
            f32 threshold, hysteresis, motionScale, motionPixels, periphery, cocScale, focus;
            u32 tileSize, maxLog2, supportedMask;
            glm::uvec2 tiles, renderSize;
            u32 refreshPeriod, frame;
        } pc{};
        pc.view = c.view;
        pc.depth = p.sampledIndex(c.depth);
        pc.velocity = p.sampledIndex(c.velocity);
        pc.prevStats = p.sampledIndex(c.prevStats);
        pc.prevRate = p.sampledIndex(c.prevRate);
        pc.outRate = p.storageIndex(c.outRate);
        pc.flags = (c.historyValid ? 1u : 0u) | (u32(c.params.mode) & 3u) << 1 | (c.depthOfField && c.view ? 8u : 0u);
        pc.threshold = c.params.threshold;
        pc.hysteresis = c.params.hysteresis;
        pc.motionScale = c.params.motionScale;
        pc.motionPixels = c.params.motionPixels;
        pc.periphery = c.params.periphery;
        pc.cocScale = c.cocScale;
        pc.focus = c.focus;
        pc.tileSize = c.tileSize;
        pc.maxLog2 = c.params.maxLog2;
        pc.supportedMask = c.params.supportedMask;
        pc.tiles = {tiles.width, tiles.height};
        pc.renderSize = {c.renderSize.width, c.renderSize.height};
        pc.refreshPeriod = c.refreshPeriod;
        pc.frame = c.frame;
        p.cmd.bindPipeline(pipeline);
        p.cmd.pushConstants(pc);
        p.cmd.dispatchThreads(tiles.width, tiles.height);
    });
}

void addAnalyzePass(rhi::RenderGraph& graph, const Pipelines& pl, const AnalyzeInputs& in, std::string_view name) {
    const Extent2D tiles = tileCount(in.renderSize, in.tileSize);
    rhi::PassBuilder pb = graph.addPass(std::string(name), rhi::PassType::Compute);
    pb.read(in.color, rhi::Access::SampledCompute).read(in.rate, rhi::Access::SampledCompute);
    if (in.combineOpaque) pb.read(in.opaqueStats, rhi::Access::SampledCompute);
    pb.overwrite(in.outStats, rhi::Access::StorageWriteCompute);
    const rhi::PipelineHandle pipeline = pl.analyze;
    const AnalyzeInputs c = in;
    pb.execute([=](rhi::PassContext& p) {
        struct {
            u32 color, rate, outStats, tileSize;
            glm::uvec2 renderSize, tiles;
            u32 flags, opaqueStats;
        } pc{p.sampledIndex(c.color),
             p.sampledIndex(c.rate),
             p.storageIndex(c.outStats),
             c.tileSize,
             {c.renderSize.width, c.renderSize.height},
             {tiles.width, tiles.height},
             (c.rateValid ? 1u : 0u) | (c.combineOpaque ? 2u : 0u),
             c.combineOpaque ? p.sampledIndex(c.opaqueStats) : 0u};
        p.cmd.bindPipeline(pipeline);
        p.cmd.pushConstants(pc);
        p.cmd.dispatch(tiles.width, tiles.height); // one 8×8 workgroup per tile
    });
}

u32 attachToPasses(rhi::RenderGraph& graph, rhi::RGTexture rate, u32 tileSize, Extent2D renderSize,
                   const std::vector<std::string>& names) {
    u32 attached = 0;
    for (u32 i = 0; i < graph.passCount(); ++i) {
        const std::string& name = graph.passName(i);
        if (graph.passType(i) != rhi::PassType::Graphics || isExcludedPass(name)) continue;
        if (std::find(names.begin(), names.end(), name) == names.end()) continue;
        const VkExtent2D e = graph.passRenderExtent(i);
        // Passes at another resolution (half-resolution particles, cube faces, shadow maps) keep full rate: the
        // tiles would not line up with what they shade.
        if (e.width != renderSize.width || e.height != renderSize.height) continue;
        graph.pass(i).shadingRate(rate, {tileSize, tileSize});
        ++attached;
    }
    return attached;
}

} // namespace vrs

void registerVrsFeatures(FeatureRegistry& registry) { registry.add(std::make_unique<vrs::VrsFeature>()); }

} // namespace ox::render
