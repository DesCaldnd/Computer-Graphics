// Physical camera depth of field: thin-lens circle of confusion from the focus distance, f-number (volume or the
// camera's CameraComponent::aperture) and focal length (volume, or derived from the vertical FOV on a 24 mm sensor);
// half-resolution gather bokeh with separate near and far fields, full-resolution composite.
#include "pp_common.hpp"

#include <cmath>

namespace ox::render::pp {

namespace {

constexpr f32 kSensorHeightMm = 24.0f;

class DepthOfFieldFeature final : public IRenderFeature {
public:
    std::string_view name() const override { return "DepthOfField"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::PostProcess); }
    i32 order() const override { return 200; }
    std::vector<std::string_view> provides() const override { return {res::kSceneColorHDR}; }
    std::vector<std::string> cvarNames() const override { return {"r.DepthOfField", "r.DOF.Quality"}; }
    bool isEnabled(const RenderSettings&, const rhi::DeviceCaps&) const override { return cvDof.get(); }

    bool initialize(FeatureInitContext& ctx) override {
        m_setup = createComputePipeline(ctx.device, "pp.dof.setup", "render/postprocess/dof_setup.comp");
        m_tiles = createComputePipeline(ctx.device, "pp.dof.tiles", "render/postprocess/dof_tiles.comp");
        m_gather = createComputePipeline(ctx.device, "pp.dof.gather", "render/postprocess/dof_gather.comp");
        m_composite = createComputePipeline(ctx.device, "pp.dof.composite", "render/postprocess/dof_composite.comp");
        return true;
    }
    void shutdown(rhi::Device& d) override {
        for (auto p : {m_setup, m_tiles, m_gather, m_composite}) d.destroy(p);
    }

    void setup(FeatureContext& ctx) override {
        const PostProcessSettings s = viewSettings(ctx);
        const CameraParams& cam = ctx.view().camera();
        if (s.focusDistance <= 0.0f || cam.projection == CameraParams::Projection::Orthographic) return;
        FrameResources& R = ctx.resources();
        const rhi::RGTexture color = R.texture(res::kSceneColorHDR);
        const rhi::RGTexture depth = R.texture(res::kDepth);
        if (!color.valid() || !depth.valid()) return;

        const Extent2D full = temporalUpscalerActive(ctx.settings(), ctx.caps()) ? ctx.outputExtent() : ctx.renderExtent();
        const Extent2D depthSize = ctx.renderExtent();
        const Extent2D half{std::max(1u, full.width / 2), std::max(1u, full.height / 2)};
        const Extent2D tiles{divUp(half.width, 8), divUp(half.height, 8)};

        const f32 fNumber = s.aperture > 0.0f ? s.aperture : viewAperture(ctx);
        const f32 focalMm = s.focalLength > 0.0f ? s.focalLength
                                                 : 0.5f * kSensorHeightMm / std::tan(0.5f * cam.verticalFov);
        const f32 f = focalMm * 1e-3f;
        const f32 S = std::max(s.focusDistance, f * 1.01f);
        const f32 apertureDiameter = f / std::max(fNumber, 0.5f);
        // Radius in full-resolution pixels per unit of (1 - S/D).
        const f32 cocScaleFull = 0.5f * apertureDiameter * f / (S - f) / (kSensorHeightMm * 1e-3f) * f32(full.height);
        const f32 maxRadiusFull = std::max(s.maxBokehSize, 0.0f) * 0.01f * f32(full.width);
        if (cocScaleFull < 0.05f || maxRadiusFull < 0.5f) return;
        const u32 rings = u32(2 + std::clamp(cvDofQuality.get(), 0, 3));

        rhi::RenderGraph& g = ctx.graph();
        const rhi::RGTexture halfTex = g.createTexture(texDesc(formats::kSceneColor, half.width, half.height, "DOF.Half"));
        const rhi::RGTexture tileTex = g.createTexture(texDesc(VK_FORMAT_R16_SFLOAT, tiles.width, tiles.height, "DOF.NearTiles"));
        const rhi::RGTexture farTex = g.createTexture(texDesc(formats::kSceneColor, half.width, half.height, "DOF.Far"));
        const rhi::RGTexture nearTex = g.createTexture(texDesc(formats::kSceneColor, half.width, half.height, "DOF.Near"));
        const rhi::RGTexture out = g.createTexture(texDesc(formats::kSceneColor, full.width, full.height, "DOF.Output"));
        const VkDeviceAddress viewAddr = ctx.viewAddress();
        const f32 focus = S;

        const rhi::PipelineHandle pSetup = m_setup, pTiles = m_tiles, pGather = m_gather, pComposite = m_composite;
        g.addPass("DOF.Setup", rhi::PassType::Compute)
            .read(color, rhi::Access::SampledCompute)
            .read(depth, rhi::Access::SampledCompute)
            .overwrite(halfTex, rhi::Access::StorageWriteCompute)
            .execute([=](rhi::PassContext& p) {
                struct {
                    u64 view;
                    u32 color, depth, dst;
                    f32 focus, cocScale, maxRadius;
                    glm::vec2 halfSize, depthSize;
                } pc{viewAddr, p.sampledIndex(color), p.sampledIndex(depth), p.storageIndex(halfTex), focus,
                     cocScaleFull * 0.5f, maxRadiusFull * 0.5f, {f32(half.width), f32(half.height)},
                     {f32(depthSize.width), f32(depthSize.height)}};
                p.cmd.bindPipeline(pSetup);
                p.cmd.pushConstants(pc);
                p.cmd.dispatchThreads(half.width, half.height);
            });
        g.addPass("DOF.Tiles", rhi::PassType::Compute)
            .read(halfTex, rhi::Access::SampledCompute)
            .overwrite(tileTex, rhi::Access::StorageWriteCompute)
            .execute([=](rhi::PassContext& p) {
                struct {
                    u32 src, dst;
                    glm::vec2 halfSize, tileSize;
                } pc{p.sampledIndex(halfTex), p.storageIndex(tileTex), {f32(half.width), f32(half.height)},
                     {f32(tiles.width), f32(tiles.height)}};
                p.cmd.bindPipeline(pTiles);
                p.cmd.pushConstants(pc);
                p.cmd.dispatchThreads(tiles.width, tiles.height);
            });
        g.addPass("DOF.Gather", rhi::PassType::Compute)
            .read(halfTex, rhi::Access::SampledCompute)
            .read(tileTex, rhi::Access::SampledCompute)
            .overwrite(farTex, rhi::Access::StorageWriteCompute)
            .overwrite(nearTex, rhi::Access::StorageWriteCompute)
            .execute([=](rhi::PassContext& p) {
                struct {
                    u32 src, tiles, outFar, outNear, rings;
                    f32 maxRadius;
                    glm::vec2 halfSize, tileSize;
                } pc{p.sampledIndex(halfTex), p.sampledIndex(tileTex), p.storageIndex(farTex), p.storageIndex(nearTex),
                     rings, maxRadiusFull * 0.5f, {f32(half.width), f32(half.height)}, {f32(tiles.width), f32(tiles.height)}};
                p.cmd.bindPipeline(pGather);
                p.cmd.pushConstants(pc);
                p.cmd.dispatchThreads(half.width, half.height);
            });
        g.addPass("DOF.Composite", rhi::PassType::Compute)
            .read(color, rhi::Access::SampledCompute)
            .read(depth, rhi::Access::SampledCompute)
            .read(farTex, rhi::Access::SampledCompute)
            .read(nearTex, rhi::Access::SampledCompute)
            .overwrite(out, rhi::Access::StorageWriteCompute)
            .execute([=](rhi::PassContext& p) {
                struct {
                    u64 view;
                    u32 color, depth, farTex, nearTex, dst;
                    f32 focus, cocScale, maxRadius;
                    glm::vec2 size, depthSize;
                } pc{viewAddr, p.sampledIndex(color), p.sampledIndex(depth), p.sampledIndex(farTex),
                     p.sampledIndex(nearTex), p.storageIndex(out), focus, cocScaleFull, maxRadiusFull,
                     {f32(full.width), f32(full.height)}, {f32(depthSize.width), f32(depthSize.height)}};
                p.cmd.bindPipeline(pComposite);
                p.cmd.pushConstants(pc);
                p.cmd.dispatchThreads(full.width, full.height);
            });
        R.setTexture(res::kSceneColorHDR, out);
    }

private:
    rhi::PipelineHandle m_setup, m_tiles, m_gather, m_composite;
};

} // namespace

std::unique_ptr<IRenderFeature> makeDepthOfFieldFeature() { return std::make_unique<DepthOfFieldFeature>(); }

} // namespace ox::render::pp
