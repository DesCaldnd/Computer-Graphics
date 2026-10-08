// "ReflectionsRT" (group "Reflections"): GGX VNDF reflection rays with one bounce of simple hit lighting, probe /
// IBL fallback above r.RayTracing.Reflections.MaxRoughness, denoised into ReflectionsSpecular.
#include "rt_internal.hpp"

namespace ox::render::rt {

namespace {

struct ReflPush {
    u64 view, scene, rt;
    u32 depth, normals, outRefl, width, height, scale, frame, spp;
    f32 maxRoughness;
    u32 flags;
};

class ReflectionsRtFeature final : public RtFeatureBase {
public:
    using RtFeatureBase::RtFeatureBase;

    std::string_view name() const override { return "ReflectionsRT"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::Lighting); }
    i32 order() const override { return 10; } // after the GI volume update (order -100)
    std::string_view exclusiveGroup() const override { return kGroupReflections; }
    i32 priority() const override { return kRtPriority; }
    std::vector<std::string_view> provides() const override { return {ox::render::res::kReflectionsSpecular}; }
    std::vector<std::string> cvarNames() const override {
        return {"r.RayTracing.Reflections", "r.RayTracing.Reflections.MaxRoughness",
                "r.RayTracing.Reflections.SamplesPerPixel", "r.RayTracing.Reflections.ResolutionScale",
                "r.RayTracing.Reflections.Denoiser"};
    }
    bool isEnabled(const RenderSettings& s, const rhi::DeviceCaps& caps) const override {
        return rtOn(s, caps) && RtSettings::fromCVars().reflections;
    }
    bool initialize(FeatureInitContext& ctx) override {
        if (!ctx.device.caps().rayTracingSupported()) return false;
        m_pipe = createComputePipeline(ctx.device, "rt.reflections", "render/raytracing/reflections.comp");
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
        const Extent2D se = scaled(in.render, st.reflectionResolutionScale);
        const u32 scale = se == in.render ? 1u : 2u;
        FrameResources& R = ctx.resources();
        const rhi::RGTexture sig = ctx.graph().createTexture(texDesc(VK_FORMAT_R16G16B16A16_SFLOAT, se, "RtReflSignal"));
        const rhi::RGTexture ddgiIrr = R.texture("RtDdgiIrradiance"), ddgiDepth = R.texture("RtDdgiDepth");
        rhi::PassBuilder pb = ctx.graph().addPass("RT.Reflections", rhi::PassType::Compute);
        pb.read(in.depth, rhi::Access::SampledCompute)
            .read(in.normals, rhi::Access::SampledCompute)
            .read(in.rtScene, rhi::Access::StorageReadCompute)
            .overwrite(sig, rhi::Access::StorageWriteCompute);
        if (ddgiIrr.valid()) pb.read(ddgiIrr, rhi::Access::SampledCompute);
        if (ddgiDepth.valid()) pb.read(ddgiDepth, rhi::Access::SampledCompute);
        const u32 spp = u32(st.reflectionSpp);
        const f32 maxRough = st.reflectionMaxRoughness;
        const u32 flags = st.shadowColored ? 1u : 0u;
        pb.execute([=, this](rhi::PassContext& p) {
            ReflPush pc{in.view, in.scene, in.rt, p.sampledIndex(in.depth), p.sampledIndex(in.normals), p.storageIndex(sig),
                        se.width, se.height, scale, in.frame, spp, maxRough, flags};
            p.cmd.bindPipeline(m_pipe);
            p.cmd.pushConstants(pc);
            p.cmd.dispatch(dispatchGroups(se.width), dispatchGroups(se.height));
        });
        const std::optional<Extent2D> up = se == in.render ? std::nullopt : std::optional<Extent2D>(in.render);
        DenoiserSettings ds = denoiserSettings(st.reflectionDenoiser, DenoiseOutput::RGBA16F, {0.2126f, 0.7152f, 0.0722f, 0.0f});
        ds.maxHistory = std::min(ds.maxHistory, 12u); // reprojection by surface motion smears mirror reflections
        ds.phiNormal = 256.0f;
        const DenoiserOutputs out = m_shared->denoiser.denoise(ctx, "RtReflections", denoiserInputs(in, sig, se), ds, up);
        R.setTexture(ox::render::res::kReflectionsSpecular, out.result);
    }

private:
    rhi::PipelineHandle m_pipe;
};

} // namespace

std::unique_ptr<IRenderFeature> makeReflectionsRtFeature(std::shared_ptr<RtShared> shared) {
    return std::make_unique<ReflectionsRtFeature>(std::move(shared));
}

} // namespace ox::render::rt
