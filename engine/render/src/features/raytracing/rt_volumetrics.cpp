// "VolumetricsRT": per-froxel ray traced light visibility for the Volumetrics feature, published as
// "VolumetricFogVisibility" (their documented hook) at InjectionPoint::Lighting before the fog lighting pass. The
// fog then sees shadows of every object in the TLAS, including off-screen casters and local lights without maps.
#include "rt_internal.hpp"

#include <oxwald/render/features/volumetrics/volumetrics.hpp>

namespace ox::render::rt {

namespace {

struct VolPush {
    u64 view, scene, rt;
    u32 outVisibility, gridX, gridY, gridZ, frame, localSamples, skyRays, flags;
};

class VolumetricsRtFeature final : public RtFeatureBase {
public:
    using RtFeatureBase::RtFeatureBase;

    std::string_view name() const override { return "VolumetricsRT"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::Lighting); }
    i32 order() const override { return volumetrics::kFogOrder - 50; }
    std::vector<std::string_view> provides() const override { return {volumetrics::kVolumetricFogVisibility}; }
    std::vector<std::string> cvarNames() const override {
        return {"r.RayTracing.Volumetrics", "r.RayTracing.Volumetrics.LocalSamples", "r.RayTracing.Volumetrics.SkyRays"};
    }
    bool isEnabled(const RenderSettings& s, const rhi::DeviceCaps& caps) const override {
        if (!rtOn(s, caps) || !RtSettings::fromCVars().volumetrics) return false;
        const ICVar* fog = CVarRegistry::instance().find("r.VolumetricFog");
        return !fog || fog->toString() == "true" || fog->toString() == "1";
    }
    bool initialize(FeatureInitContext& ctx) override {
        if (!ctx.device.caps().rayTracingSupported()) return false;
        m_pipe = createComputePipeline(ctx.device, "rt.volumetricVisibility", "render/raytracing/volumetric_visibility.comp");
        return true;
    }
    void shutdown(rhi::Device& d) override {
        if (m_pipe) d.destroy(m_pipe);
    }

    void setup(FeatureContext& ctx) override {
        RtViewInputs in;
        if (!gather(ctx, in)) return;
        if (!volumetrics::fogActive(ctx.snapshot())) return;
        const RtSettings& st = rts();
        const volumetrics::FroxelGrid grid = volumetrics::froxelGridFromCVars(&ctx.snapshot());
        rhi::TextureDesc td;
        td.type = rhi::TextureType::Tex3D;
        td.format = volumetrics::kVisibilityFormat;
        td.width = grid.x;
        td.height = grid.y;
        td.depth = grid.z;
        td.usage = rhi::TextureUsage::None;
        td.name = "VolumetricFogVisibility";
        const rhi::RGTexture vis = ctx.graph().createTexture(td);
        const rhi::RGBuffer clusters = ctx.resources().buffer(ox::render::res::kLightClusters);
        rhi::PassBuilder pb = ctx.graph().addPass("RT.VolumetricVisibility", rhi::PassType::Compute);
        pb.read(in.rtScene, rhi::Access::StorageReadCompute).overwrite(vis, rhi::Access::StorageWriteCompute);
        if (clusters.valid()) pb.read(clusters, rhi::Access::StorageReadCompute);
        const u32 local = u32(st.volumetricLocalSamples), sky = u32(st.volumetricSkyRays);
        const u32 flags = st.shadowColored ? 1u : 0u;
        pb.execute([=, this](rhi::PassContext& p) {
            VolPush pc{in.view, in.scene, in.rt, p.storageIndex(vis), grid.x, grid.y, grid.z, in.frame, local, sky, flags};
            p.cmd.bindPipeline(m_pipe);
            p.cmd.pushConstants(pc);
            p.cmd.dispatch(dispatchGroups(grid.x, 4), dispatchGroups(grid.y, 4), dispatchGroups(grid.z, 4));
        });
        ctx.resources().setTexture(volumetrics::kVolumetricFogVisibility, vis);
    }

private:
    rhi::PipelineHandle m_pipe;
};

} // namespace

std::unique_ptr<IRenderFeature> makeVolumetricsRtFeature(std::shared_ptr<RtShared> shared) {
    return std::make_unique<VolumetricsRtFeature>(std::move(shared));
}

} // namespace ox::render::rt
