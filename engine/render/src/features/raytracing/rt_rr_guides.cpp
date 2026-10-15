// "RayReconstructionGuidesRT" (Lighting): diffuse / specular albedo guides of DLSS Ray Reconstruction at render
// resolution, from one short primary ray per pixel against the TLAS (rr_guides.comp). Runs only in frames where the
// DLSSRR upscaler denoises the view (rayReconstructionRunsThisFrame); the SVGF path never pays for it.
#include "rt_internal.hpp"

#include <cmath>

namespace ox::render::rt {

namespace {

struct GuidesPush {
    u64 view, scene, rt;
    u32 depth, normals, outDiffuse, outSpecular, width, height;
    f32 spreadAngle;
};

class RayReconstructionGuidesFeature final : public RtFeatureBase {
public:
    using RtFeatureBase::RtFeatureBase;

    std::string_view name() const override { return "RayReconstructionGuidesRT"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::Lighting); }
    i32 order() const override { return 50; }
    std::vector<std::string_view> provides() const override {
        return {ox::render::res::kRrDiffuseAlbedo, ox::render::res::kRrSpecularAlbedo};
    }
    std::vector<std::string> cvarNames() const override { return {"r.Denoiser"}; }
    bool isEnabled(const RenderSettings& s, const rhi::DeviceCaps& caps) const override {
        return rtOn(s, caps) && rayReconstructionSelected(s, caps);
    }
    bool initialize(FeatureInitContext& ctx) override {
        if (!ctx.device.caps().rayTracingSupported()) return false;
        m_pipe = createComputePipeline(ctx.device, "rt.rrGuides", "render/raytracing/rr_guides.comp");
        return true;
    }
    void shutdown(rhi::Device& d) override {
        if (m_pipe) d.destroy(m_pipe);
        m_pipe = {};
    }

    void setup(FeatureContext& ctx) override {
        if (!rayReconstructionRunsThisFrame(ctx.view())) return;
        RtViewInputs in;
        if (!gather(ctx, in)) return;
        rhi::RenderGraph& graph = ctx.graph();
        const rhi::RGTexture diffuse = graph.createTexture(texDesc(VK_FORMAT_R8G8B8A8_UNORM, in.render, "RR.DiffuseAlbedo"));
        const rhi::RGTexture specular = graph.createTexture(texDesc(VK_FORMAT_R8G8B8A8_UNORM, in.render, "RR.SpecularAlbedo"));
        // Angle between neighbouring pixel rays (texture LOD of the hit, ray cones).
        const f32 py = std::abs(ctx.view().unjitteredProj()[1][1]);
        const f32 spread = 2.0f / (std::max(py, 1e-4f) * f32(std::max(in.render.height, 1u)));
        graph.addPass("RT.RRGuides", rhi::PassType::Compute)
            .read(in.depth, rhi::Access::SampledCompute)
            .read(in.normals, rhi::Access::SampledCompute)
            .read(in.rtScene, rhi::Access::StorageReadCompute)
            .overwrite(diffuse, rhi::Access::StorageWriteCompute)
            .overwrite(specular, rhi::Access::StorageWriteCompute)
            .execute([=, this](rhi::PassContext& p) {
                GuidesPush pc{in.view, in.scene, in.rt, p.sampledIndex(in.depth), p.sampledIndex(in.normals),
                              p.storageIndex(diffuse), p.storageIndex(specular), in.render.width, in.render.height, spread};
                p.cmd.bindPipeline(m_pipe);
                p.cmd.pushConstants(pc);
                p.cmd.dispatch(dispatchGroups(in.render.width), dispatchGroups(in.render.height));
            });
        ctx.resources().setTexture(ox::render::res::kRrDiffuseAlbedo, diffuse);
        ctx.resources().setTexture(ox::render::res::kRrSpecularAlbedo, specular);
    }

private:
    rhi::PipelineHandle m_pipe;
};

} // namespace

std::unique_ptr<IRenderFeature> makeRayReconstructionGuidesFeature(std::shared_ptr<RtShared> shared) {
    return std::make_unique<RayReconstructionGuidesFeature>(std::move(shared));
}

} // namespace ox::render::rt
