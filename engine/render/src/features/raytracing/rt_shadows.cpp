// "ShadowsRT" (group "Shadows", replaces ShadowsRaster when ray tracing is on): ray traced sun shadows into an
// RGBA8 ShadowMask (coloured transmission in rgb, view flag OX_VIEW_RT_SHADOW_MASK_RGB) and local light shadows as
// screen-space visibility channels (GpuShadow kind 2) of a persistent per-view RGBA8 texture: up to 4 explicit
// lights, or every shadowed local light through the ReSTIR DI ratio estimator. Both signals are denoised.
#include "rt_internal.hpp"

#include <oxwald/render/snapshot.hpp>

#include <algorithm>
#include <unordered_map>

namespace ox::render::rt {

namespace {

constexpr u32 kViewFlagRtShadowMaskRgb = 1u << 8; // OX_VIEW_RT_SHADOW_MASK_RGB in lighting.glsl
constexpr u32 kShadowKindScreenMask = 2;         // OX_SHADOW_KIND_SCREEN_MASK
constexpr u32 kReservoirBytes = 32;

struct ShadowsPush {
    u64 view, scene, rt, reservoirs;
    u32 depth, normals, outSun, outLocal, width, height, scale, frame, spp, localCount, flags;
    glm::uvec4 localLights;
};

struct RestirInitialPush {
    u64 view, scene, prevRes, curRes, lightList;
    u32 depth, normals, velocity, lightCount, width, height, scale, frame, candidates, flags;
};
static_assert(sizeof(RestirInitialPush) <= 128);

struct RestirSpatialPush {
    u64 view, scene, src, dst;
    u32 depth, normals, width, height, scale, frame, samples;
    f32 radius;
};

struct ShadowRtViewState final : IFeatureViewState {
    rhi::TextureHandle localMask;
    rhi::BufferHandle reservoirs[3]; // history ping-pong (0/1) + spatial temp (2)
    u64 reservoirBytes = 0;
    u32 parity = 0;
    bool reservoirsValid = false;
    void release(rhi::Device& d) override {
        if (localMask) d.destroy(localMask);
        for (auto& b : reservoirs) {
            if (b) d.destroy(b);
            b = {};
        }
        localMask = {};
    }
};

class ShadowsRtFeature final : public RtFeatureBase {
public:
    using RtFeatureBase::RtFeatureBase;

    std::string_view name() const override { return "ShadowsRT"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::Shadows); }
    std::string_view exclusiveGroup() const override { return kGroupShadows; }
    i32 priority() const override { return kRtPriority; }
    std::vector<std::string_view> provides() const override { return {ox::render::res::kShadowMask, res::kLocalLightShadows}; }
    std::vector<std::string> cvarNames() const override {
        return {"r.RayTracing.Shadows", "r.RayTracing.Shadows.SamplesPerPixel", "r.RayTracing.Shadows.MaxLocalLights",
                "r.RayTracing.Shadows.ReSTIR", "r.RayTracing.Shadows.Colored", "r.RayTracing.Shadows.Denoiser",
                "r.RayTracing.Shadows.ResolutionScale"};
    }
    bool isEnabled(const RenderSettings& s, const rhi::DeviceCaps& caps) const override {
        return rtOn(s, caps) && s.shadows && RtSettings::fromCVars().shadows;
    }

    bool initialize(FeatureInitContext& ctx) override {
        if (!ctx.device.caps().rayTracingSupported()) return false;
        m_trace = createComputePipeline(ctx.device, "rt.shadows", "render/raytracing/shadows.comp");
        m_restirInitial = createComputePipeline(ctx.device, "rt.restir.initial", "render/raytracing/restir_initial.comp");
        m_restirSpatial = createComputePipeline(ctx.device, "rt.restir.spatial", "render/raytracing/restir_spatial.comp");
        return m_shared->acquireDenoiser(ctx.device);
    }
    void shutdown(rhi::Device& d) override {
        for (rhi::PipelineHandle p : {m_trace, m_restirInitial, m_restirSpatial}) {
            if (p) d.destroy(p);
        }
        m_shared->releaseDenoiser(d);
    }

    void setup(FeatureContext& ctx) override {
        RtViewInputs in;
        if (!gather(ctx, in)) return;
        rhi::Device& dev = ctx.device();
        rhi::RenderGraph& graph = ctx.graph();
        FrameResources& R = ctx.resources();
        FrameState& fs = frameState(ctx);
        GpuViewConstants& c = ctx.viewConstants();
        const RtSettings& st = rts();
        const Extent2D se = scaled(in.render, st.shadowResolutionScale);
        const u32 scale = se == in.render ? 1u : 2u;
        ShadowRtViewState& vs = ctx.viewState<ShadowRtViewState>();

        // --- which lights cast shadows ---
        std::unordered_map<u32, const SnapshotLight*> byEntity;
        for (const SnapshotLight& l : ctx.snapshot().lights) byEntity[l.entityId] = &l;
        auto casts = [&](const GpuLight& g) {
            auto it = byEntity.find(g.entityId);
            return it != byEntity.end() && it->second->light.castShadows;
        };
        const bool sunShadows = c.sunLight >= 0 && casts(fs.lights[usize(c.sunLight)]);
        std::vector<u32> locals;
        for (u32 i = fs.directionalCount; i < fs.lights.size(); ++i) {
            if (casts(fs.lights[i])) locals.push_back(i);
        }
        const bool restir = st.shadowReSTIR && !locals.empty();
        if (!restir) {
            // Most important lights first: intensity over squared distance to the camera (range-clamped).
            const glm::vec3 cam = ctx.view().camera().position();
            auto score = [&](u32 i) {
                const GpuLight& g = fs.lights[i];
                const f32 d = std::max(glm::length(g.position - cam) - g.range * 0.5f, 0.5f);
                return glm::dot(g.color, glm::vec3(0.2126f, 0.7152f, 0.0722f)) / (d * d);
            };
            std::stable_sort(locals.begin(), locals.end(), [&](u32 a, u32 b) { return score(a) > score(b); });
            locals.resize(std::min<usize>(locals.size(), usize(st.shadowMaxLocalLights)));
        }
        const bool anyLocal = !locals.empty();

        // --- persistent local visibility mask (bindless index goes into GpuShadow) ---
        rhi::RGTexture maskRG;
        if (anyLocal) {
            if (!vs.localMask || dev.desc(vs.localMask).width != in.render.width || dev.desc(vs.localMask).height != in.render.height) {
                if (vs.localMask) dev.destroy(vs.localMask);
                rhi::TextureDesc td = texDesc(VK_FORMAT_R8G8B8A8_UNORM, in.render, "rt.localLightShadows");
                td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage | rhi::TextureUsage::TransferDst;
                vs.localMask = dev.createTexture(td);
            }
            maskRG = importPersistent(graph, dev, vs.localMask);
            const u32 maskIndex = dev.sampledIndex(vs.localMask);
            if (restir) {
                GpuShadow sh;
                sh.kind = kShadowKindScreenMask;
                sh.atlasRect = {0.0f, 0.0f, 0.0f, 0.0f};
                sh.cubeLayer = maskIndex;
                const i32 index = i32(fs.shadows.size());
                fs.shadows.push_back(sh);
                for (u32 li : locals) fs.lights[li].shadowIndex = index;
            } else {
                for (u32 k = 0; k < locals.size(); ++k) {
                    GpuShadow sh;
                    sh.kind = kShadowKindScreenMask;
                    sh.atlasRect = {f32(k), 0.0f, 0.0f, 0.0f};
                    sh.cubeLayer = maskIndex;
                    fs.lights[locals[k]].shadowIndex = i32(fs.shadows.size());
                    fs.shadows.push_back(sh);
                }
            }
            ctx.stats().shadowedLights += u32(locals.size());
        }
        c.flags |= kViewFlagRtShadowMaskRgb;

        // --- ReSTIR DI reservoirs ---
        rhi::RGBuffer resCur, resPrev, resOut;
        if (restir) {
            const u64 bytes = u64(se.width) * se.height * kReservoirBytes;
            if (vs.reservoirBytes != bytes) {
                for (auto& b : vs.reservoirs) {
                    if (b) dev.destroy(b);
                    b = dev.createBuffer({bytes, rhi::BufferUsage::Storage, rhi::MemoryUsage::GpuOnly, "rt.restir.reservoirs"});
                }
                vs.reservoirBytes = bytes;
                vs.reservoirsValid = false;
            }
            const rhi::BufferDesc bd{bytes, rhi::BufferUsage::Storage, rhi::MemoryUsage::GpuOnly, "Reservoirs"};
            vs.parity ^= 1u;
            resPrev = graph.importBuffer(vs.reservoirs[vs.parity ^ 1u], bd, {rhi::Access::General, rhi::Access::Undefined});
            resCur = graph.importBuffer(vs.reservoirs[2], bd, {rhi::Access::General, rhi::Access::Undefined});
            resOut = graph.importBuffer(vs.reservoirs[vs.parity], bd, {rhi::Access::General, rhi::Access::Undefined});
            const VkDeviceAddress list = ctx.upload(std::span<const u32>(locals));
            const u32 lightCount = u32(locals.size());
            const bool temporal = vs.reservoirsValid && !ctx.view().cameraCut();
            const RtViewInputs i = in;
            graph.addPass("RT.ReSTIR.Initial", rhi::PassType::Compute)
                .read(in.depth, rhi::Access::SampledCompute)
                .read(in.normals, rhi::Access::SampledCompute)
                .read(in.velocity, rhi::Access::SampledCompute)
                .read(resPrev, rhi::Access::StorageReadCompute)
                .overwrite(resCur, rhi::Access::StorageWriteCompute)
                .execute([=, this](rhi::PassContext& p) {
                    RestirInitialPush pc{i.view, i.scene, p.address(resPrev), p.address(resCur), list,
                                         p.sampledIndex(i.depth), p.sampledIndex(i.normals), p.sampledIndex(i.velocity),
                                         lightCount, se.width, se.height, scale, i.frame, 32u, temporal ? 1u : 0u};
                    p.cmd.bindPipeline(m_restirInitial);
                    p.cmd.pushConstants(pc);
                    p.cmd.dispatch(dispatchGroups(se.width), dispatchGroups(se.height));
                });
            graph.addPass("RT.ReSTIR.Spatial", rhi::PassType::Compute)
                .read(in.depth, rhi::Access::SampledCompute)
                .read(in.normals, rhi::Access::SampledCompute)
                .read(resCur, rhi::Access::StorageReadCompute)
                .overwrite(resOut, rhi::Access::StorageWriteCompute)
                .execute([=, this](rhi::PassContext& p) {
                    RestirSpatialPush pc{i.view, i.scene, p.address(resCur), p.address(resOut), p.sampledIndex(i.depth),
                                         p.sampledIndex(i.normals), se.width, se.height, scale, i.frame, 4u, 16.0f / f32(scale)};
                    p.cmd.bindPipeline(m_restirSpatial);
                    p.cmd.pushConstants(pc);
                    p.cmd.dispatch(dispatchGroups(se.width), dispatchGroups(se.height));
                });
            vs.reservoirsValid = true;
        } else {
            vs.reservoirsValid = false;
        }

        // --- trace ---
        const rhi::RGTexture sunSig = graph.createTexture(texDesc(VK_FORMAT_R16G16B16A16_SFLOAT, se, "RtShadowSun"));
        rhi::RGTexture localSig;
        if (anyLocal) localSig = graph.createTexture(texDesc(VK_FORMAT_R16G16B16A16_SFLOAT, se, "RtShadowLocal"));
        {
            rhi::PassBuilder pb = graph.addPass("RT.Shadows", rhi::PassType::Compute);
            pb.read(in.depth, rhi::Access::SampledCompute)
                .read(in.normals, rhi::Access::SampledCompute)
                .read(in.rtScene, rhi::Access::StorageReadCompute)
                .overwrite(sunSig, rhi::Access::StorageWriteCompute);
            if (anyLocal) pb.overwrite(localSig, rhi::Access::StorageWriteCompute);
            if (restir) pb.read(resOut, rhi::Access::StorageReadCompute);
            glm::uvec4 lightIds{0u};
            for (u32 k = 0; k < std::min<usize>(locals.size(), 4); ++k) lightIds[k] = locals[k];
            const u32 flags = (st.shadowColored ? 1u : 0u) | (sunShadows ? 2u : 0u) | (restir ? 4u : 0u) |
                              (anyLocal && !restir ? 8u : 0u);
            const u32 localCount = restir ? 0u : u32(locals.size());
            const RtViewInputs i = in;
            const u32 spp = u32(st.shadowSpp);
            pb.execute([=, this](rhi::PassContext& p) {
                ShadowsPush pc{};
                pc.view = i.view;
                pc.scene = i.scene;
                pc.rt = i.rt;
                pc.reservoirs = restir ? p.address(resOut) : 0;
                pc.depth = p.sampledIndex(i.depth);
                pc.normals = p.sampledIndex(i.normals);
                pc.outSun = p.storageIndex(sunSig);
                pc.outLocal = anyLocal ? p.storageIndex(localSig) : pc.outSun;
                pc.width = se.width;
                pc.height = se.height;
                pc.scale = scale;
                pc.frame = i.frame;
                pc.spp = spp;
                pc.localCount = localCount;
                pc.flags = flags;
                pc.localLights = lightIds;
                p.cmd.bindPipeline(m_trace);
                p.cmd.pushConstants(pc);
                p.cmd.dispatch(dispatchGroups(se.width), dispatchGroups(se.height));
            });
        }

        // --- denoise + publish ---
        const std::optional<Extent2D> up = se == in.render ? std::nullopt : std::optional<Extent2D>(in.render);
        const DenoiserOutputs sun = m_shared->denoiser.denoise(
            ctx, "RtShadowSun", denoiserInputs(in, sunSig, se),
            denoiserSettings(st.shadowDenoiser, DenoiseOutput::RGBA8, {1.0f / 3, 1.0f / 3, 1.0f / 3, 0.0f}), up);
        R.setTexture(ox::render::res::kShadowMask, sun.result);
        if (anyLocal) {
            const DenoiserOutputs local = m_shared->denoiser.denoise(
                ctx, "RtShadowLocal", denoiserInputs(in, localSig, se),
                denoiserSettings(st.shadowDenoiser, DenoiseOutput::RGBA8, {0.25f, 0.25f, 0.25f, 0.25f}), up);
            const rhi::RGTexture src = local.result;
            graph.addPass("RT.Shadows.PublishLocal", rhi::PassType::Transfer)
                .read(src, rhi::Access::TransferRead)
                .overwrite(maskRG, rhi::Access::TransferWrite)
                .execute([src, maskRG](rhi::PassContext& p) { p.cmd.copyTexture(p.texture(src), p.texture(maskRG)); });
            // The forward pass reads every published shadow texture: reuse the ShadowAtlas slot for the barrier.
            if (!R.texture(ox::render::res::kShadowAtlas).valid()) R.setTexture(ox::render::res::kShadowAtlas, maskRG);
            R.setTexture(res::kLocalLightShadows, maskRG);
        }
    }

private:
    rhi::PipelineHandle m_trace, m_restirInitial, m_restirSpatial;
};

} // namespace

std::unique_ptr<IRenderFeature> makeShadowsRtFeature(std::shared_ptr<RtShared> shared) {
    return std::make_unique<ShadowsRtFeature>(std::move(shared));
}

} // namespace ox::render::rt
