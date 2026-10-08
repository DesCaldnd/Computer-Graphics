// "TranslucencyRT": ray traced refraction for refractive materials (Snell, internal bounces, TIR, Beer-Lambert,
// reflections of off-screen objects, coloured shadows in the hit lighting). Runs at the end of
// InjectionPoint::Translucency and replaces the pixels whose nearest translucent surface is refractive; the raster
// Translucency feature keeps drawing transparent (alpha blended) materials, particles and water.
#include "rt_internal.hpp"

namespace ox::render::rt {

namespace {

struct TransPush {
    u64 view, scene, rt;
    u32 depth, sceneColor, width, height, frame, maxBounces, flags;
};

class TranslucencyRtFeature final : public RtFeatureBase {
public:
    using RtFeatureBase::RtFeatureBase;

    std::string_view name() const override { return "TranslucencyRT"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::Translucency); }
    i32 order() const override { return 1000; } // after the raster translucency
    std::vector<std::string_view> provides() const override { return {ox::render::res::kSceneColorHDR}; }
    std::vector<std::string> cvarNames() const override {
        return {"r.RayTracing.Translucency", "r.RayTracing.Translucency.MaxBounces"};
    }
    bool isEnabled(const RenderSettings& s, const rhi::DeviceCaps& caps) const override {
        return rayTracedRefractionActive(s, caps);
    }
    bool initialize(FeatureInitContext& ctx) override {
        if (!ctx.device.caps().rayTracingSupported()) return false;
        m_pipe = createComputePipeline(ctx.device, "rt.translucency", "render/raytracing/translucency.comp");
        return true;
    }
    void shutdown(rhi::Device& d) override {
        if (m_pipe) d.destroy(m_pipe);
    }

    void setup(FeatureContext& ctx) override {
        RtViewInputs in;
        if (!gather(ctx, in)) return;
        const RtSettings& st = rts();
        const rhi::RGTexture hdr = ctx.resources().texture(ox::render::res::kSceneColorHDR);
        if (!hdr.valid()) return;
        const u32 bounces = u32(st.translucencyMaxBounces);
        const u32 flags = st.shadowColored ? 1u : 0u;
        const Extent2D e = in.render;
        ctx.graph()
            .addPass("RT.Translucency", rhi::PassType::Compute)
            .read(in.depth, rhi::Access::SampledCompute)
            .read(in.rtScene, rhi::Access::StorageReadCompute)
            .write(hdr, rhi::Access::StorageWriteCompute)
            .execute([=, this](rhi::PassContext& p) {
                TransPush pc{in.view, in.scene, in.rt, p.sampledIndex(in.depth), p.storageIndex(hdr), e.width, e.height,
                             in.frame, bounces, flags};
                p.cmd.bindPipeline(m_pipe);
                p.cmd.pushConstants(pc);
                p.cmd.dispatch(dispatchGroups(e.width), dispatchGroups(e.height));
            });
    }

private:
    rhi::PipelineHandle m_pipe;
};

} // namespace

bool rayTracedRefractionActive(const RenderSettings& settings, const rhi::DeviceCaps& caps) {
    return rayTracingActive(settings, caps) && RtSettings::fromCVars().translucency &&
           FeatureRegistry::toggle("TranslucencyRT").get();
}

std::unique_ptr<IRenderFeature> makeTranslucencyRtFeature(std::shared_ptr<RtShared> shared) {
    return std::make_unique<TranslucencyRtFeature>(std::move(shared));
}

} // namespace ox::render::rt
