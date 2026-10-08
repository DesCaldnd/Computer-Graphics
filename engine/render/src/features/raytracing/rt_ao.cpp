// "AmbientOcclusionRT" (group "AO"): RTAO with cosine hemisphere rays, denoised into the R8 AO input.
#include "rt_internal.hpp"

namespace ox::render::rt {

namespace {

struct AoPush {
    u64 view, scene, rt;
    u32 depth, normals, outAo, width, height, scale, frame, spp;
    f32 radius;
};

class AmbientOcclusionRtFeature final : public RtFeatureBase {
public:
    using RtFeatureBase::RtFeatureBase;

    std::string_view name() const override { return "AmbientOcclusionRT"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::Lighting); }
    std::string_view exclusiveGroup() const override { return kGroupAO; }
    i32 priority() const override { return kRtPriority; }
    std::vector<std::string_view> provides() const override { return {ox::render::res::kAO}; }
    std::vector<std::string> cvarNames() const override {
        return {"r.RayTracing.AO", "r.RayTracing.AO.Radius", "r.RayTracing.AO.SamplesPerPixel",
                "r.RayTracing.AO.ResolutionScale", "r.RayTracing.AO.Denoiser"};
    }
    bool isEnabled(const RenderSettings& s, const rhi::DeviceCaps& caps) const override {
        return rtOn(s, caps) && RtSettings::fromCVars().ao;
    }
    bool initialize(FeatureInitContext& ctx) override {
        if (!ctx.device.caps().rayTracingSupported()) return false;
        m_pipe = createComputePipeline(ctx.device, "rt.ao", "render/raytracing/ao.comp");
        return m_shared->acquireDenoiser(ctx.device);
    }
    void shutdown(rhi::Device& d) override {
        if (m_pipe) d.destroy(m_pipe);
        m_shared->releaseDenoiser(d);
    }

    void setup(FeatureContext& ctx) override {
        RtViewInputs in;
        if (!gather(ctx, in)) return;
        const RtSettings& st = rts();
        const Extent2D se = scaled(in.render, st.aoResolutionScale);
        const u32 scale = se == in.render ? 1u : 2u;
        const rhi::RGTexture sig = ctx.graph().createTexture(texDesc(VK_FORMAT_R16G16B16A16_SFLOAT, se, "RtAoSignal"));
        const u32 spp = u32(st.aoSpp);
        const f32 radius = st.aoRadius;
        ctx.graph()
            .addPass("RT.AO", rhi::PassType::Compute)
            .read(in.depth, rhi::Access::SampledCompute)
            .read(in.normals, rhi::Access::SampledCompute)
            .read(in.rtScene, rhi::Access::StorageReadCompute)
            .overwrite(sig, rhi::Access::StorageWriteCompute)
            .execute([=, this](rhi::PassContext& p) {
                AoPush pc{in.view, in.scene, in.rt, p.sampledIndex(in.depth), p.sampledIndex(in.normals), p.storageIndex(sig),
                          se.width, se.height, scale, in.frame, spp, radius};
                p.cmd.bindPipeline(m_pipe);
                p.cmd.pushConstants(pc);
                p.cmd.dispatch(dispatchGroups(se.width), dispatchGroups(se.height));
            });
        const std::optional<Extent2D> up = se == in.render ? std::nullopt : std::optional<Extent2D>(in.render);
        const DenoiserOutputs out = m_shared->denoiser.denoise(
            ctx, "RtAO", denoiserInputs(in, sig, se), denoiserSettings(st.aoDenoiser, DenoiseOutput::R8, {1, 0, 0, 0}), up);
        ctx.resources().setTexture(ox::render::res::kAO, out.result);
    }

private:
    rhi::PipelineHandle m_pipe;
};

} // namespace

std::unique_ptr<IRenderFeature> makeAmbientOcclusionRtFeature(std::shared_ptr<RtShared> shared) {
    return std::make_unique<AmbientOcclusionRtFeature>(std::move(shared));
}

} // namespace ox::render::rt
