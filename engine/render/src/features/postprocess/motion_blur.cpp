// Motion blur (McGuire et al. reconstruction filter): tile max velocity (16×16) → neighbour max → gather along the
// dominant velocity with depth-aware sample weights. Velocity contains camera and object motion (and skinning).
#include "pp_common.hpp"

namespace ox::render::pp {

namespace {

class MotionBlurFeature final : public IRenderFeature {
public:
    std::string_view name() const override { return "MotionBlur"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::PostProcess); }
    i32 order() const override { return 300; }
    std::vector<std::string_view> provides() const override { return {res::kSceneColorHDR}; }
    std::vector<std::string> cvarNames() const override { return {"r.MotionBlur", "r.MotionBlur.Quality"}; }
    bool isEnabled(const RenderSettings&, const rhi::DeviceCaps&) const override { return cvMotionBlur.get(); }

    bool initialize(FeatureInitContext& ctx) override {
        m_tiles = createComputePipeline(ctx.device, "pp.mb.tiles", "render/postprocess/mb_tiles.comp");
        m_neighbor = createComputePipeline(ctx.device, "pp.mb.neighbor", "render/postprocess/mb_neighbor.comp");
        m_gather = createComputePipeline(ctx.device, "pp.mb.gather", "render/postprocess/mb_gather.comp");
        return true;
    }
    void shutdown(rhi::Device& d) override {
        for (auto p : {m_tiles, m_neighbor, m_gather}) d.destroy(p);
    }

    void setup(FeatureContext& ctx) override {
        const PostProcessSettings s = viewSettings(ctx);
        if (s.motionBlurAmount <= 0.0f || s.motionBlurMax <= 0.0f || ctx.view().cameraCut()) return;
        FrameResources& R = ctx.resources();
        const rhi::RGTexture color = R.texture(res::kSceneColorHDR);
        const rhi::RGTexture velocity = R.texture(res::kVelocity);
        const rhi::RGTexture depth = R.texture(res::kDepth);
        if (!color.valid() || !velocity.valid() || !depth.valid()) return;
        const Extent2D size = temporalUpscalerActive(ctx.settings(), ctx.caps()) ? ctx.outputExtent() : ctx.renderExtent();
        const Extent2D vsize = ctx.renderExtent();
        const Extent2D tiles{divUp(vsize.width, 16), divUp(vsize.height, 16)};
        const f32 shutter = std::clamp(s.motionBlurAmount, 0.0f, 1.0f);
        const f32 maxLength = s.motionBlurMax * 0.01f * f32(size.width);
        const u32 samples = u32(4 + 4 * std::clamp(cvMotionBlurQuality.get(), 0, 3));

        rhi::RenderGraph& g = ctx.graph();
        const rhi::RGTexture tileMax = g.createTexture(texDesc(formats::kVelocity, tiles.width, tiles.height, "MotionBlur.TileMax"));
        const rhi::RGTexture neighbor = g.createTexture(texDesc(formats::kVelocity, tiles.width, tiles.height, "MotionBlur.NeighborMax"));
        const rhi::RGTexture out = g.createTexture(texDesc(formats::kSceneColor, size.width, size.height, "MotionBlur.Output"));
        const VkDeviceAddress viewAddr = ctx.viewAddress();
        const rhi::PipelineHandle pTiles = m_tiles, pNeighbor = m_neighbor, pGather = m_gather;
        const glm::vec2 colorSize{f32(size.width), f32(size.height)}, velSize{f32(vsize.width), f32(vsize.height)};
        const glm::vec2 tileCount{f32(tiles.width), f32(tiles.height)};
        g.addPass("MotionBlur.TileMax", rhi::PassType::Compute)
            .read(velocity, rhi::Access::SampledCompute)
            .overwrite(tileMax, rhi::Access::StorageWriteCompute)
            .execute([=](rhi::PassContext& p) {
                struct {
                    u32 velocity, dst;
                    f32 shutter, maxLength;
                    glm::vec2 velocitySize, colorSize, tileCount;
                } pc{p.sampledIndex(velocity), p.storageIndex(tileMax), shutter, maxLength, velSize, colorSize, tileCount};
                p.cmd.bindPipeline(pTiles);
                p.cmd.pushConstants(pc);
                p.cmd.dispatchThreads(tiles.width, tiles.height);
            });
        g.addPass("MotionBlur.NeighborMax", rhi::PassType::Compute)
            .read(tileMax, rhi::Access::SampledCompute)
            .overwrite(neighbor, rhi::Access::StorageWriteCompute)
            .execute([=](rhi::PassContext& p) {
                struct {
                    u32 src, dst;
                    glm::vec2 tileCount;
                } pc{p.sampledIndex(tileMax), p.storageIndex(neighbor), tileCount};
                p.cmd.bindPipeline(pNeighbor);
                p.cmd.pushConstants(pc);
                p.cmd.dispatchThreads(tiles.width, tiles.height);
            });
        g.addPass("MotionBlur.Gather", rhi::PassType::Compute)
            .read(color, rhi::Access::SampledCompute)
            .read(velocity, rhi::Access::SampledCompute)
            .read(depth, rhi::Access::SampledCompute)
            .read(neighbor, rhi::Access::SampledCompute)
            .overwrite(out, rhi::Access::StorageWriteCompute)
            .execute([=](rhi::PassContext& p) {
                struct {
                    u64 view;
                    u32 color, velocity, depth, neighborMax, dst, samples;
                    f32 shutter, maxLength;
                    glm::vec2 size, velocitySize, tileCount;
                } pc{viewAddr, p.sampledIndex(color), p.sampledIndex(velocity), p.sampledIndex(depth),
                     p.sampledIndex(neighbor), p.storageIndex(out), samples, shutter, maxLength, colorSize, velSize,
                     tileCount};
                p.cmd.bindPipeline(pGather);
                p.cmd.pushConstants(pc);
                p.cmd.dispatchThreads(size.width, size.height);
            });
        R.setTexture(res::kSceneColorHDR, out);
    }

private:
    rhi::PipelineHandle m_tiles, m_neighbor, m_gather;
};

} // namespace

std::unique_ptr<IRenderFeature> makeMotionBlurFeature() { return std::make_unique<MotionBlurFeature>(); }

} // namespace ox::render::pp
