// Physically based bloom: threshold-less dual-filter downsample/upsample chain (energy conserving). Publishes the
// half-resolution bloom image (PostProcess.Bloom); the HDR composite lerps it in with the intensity and adds the lens
// dirt mask.
#include "pp_common.hpp"

#include <format>

namespace ox::render::pp {

namespace {

class BloomFeature final : public IRenderFeature {
public:
    std::string_view name() const override { return "Bloom"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::PostProcess); }
    i32 order() const override { return 400; }
    std::vector<std::string_view> provides() const override { return {kBloomResult}; }
    std::vector<std::string> cvarNames() const override { return {"r.Bloom", "r.Bloom.Quality", "r.Bloom.Intensity"}; }
    bool isEnabled(const RenderSettings&, const rhi::DeviceCaps&) const override { return cvBloom.get(); }

    bool initialize(FeatureInitContext& ctx) override {
        m_down = createComputePipeline(ctx.device, "pp.bloom.down", "render/postprocess/bloom_down.comp");
        m_up = createComputePipeline(ctx.device, "pp.bloom.up", "render/postprocess/bloom_up.comp");
        return true;
    }
    void shutdown(rhi::Device& d) override {
        d.destroy(m_down);
        d.destroy(m_up);
    }

    void setup(FeatureContext& ctx) override {
        const PostProcessSettings s = viewSettings(ctx);
        if (s.bloomIntensity <= 0.0f) return;
        FrameResources& R = ctx.resources();
        const rhi::RGTexture color = R.texture(res::kSceneColorHDR);
        if (!color.valid()) return;
        rhi::RenderGraph& g = ctx.graph();
        // SceneColorHDR is at output resolution after a temporal upscaler.
        const Extent2D src = temporalUpscalerActive(ctx.settings(), ctx.caps()) ? ctx.outputExtent() : ctx.renderExtent();
        const u32 wanted = u32(4 + std::clamp(cvBloomQuality.get(), 0, 4));
        std::vector<rhi::RGTexture> down;
        std::vector<Extent2D> sizes;
        u32 w = src.width, h = src.height;
        for (u32 i = 0; i < wanted; ++i) {
            w = std::max(1u, w / 2);
            h = std::max(1u, h / 2);
            if (i > 0 && (w < 4 || h < 4)) break;
            sizes.push_back({w, h});
            down.push_back(g.createTexture(texDesc(formats::kSceneColor, w, h, "Bloom.Down")));
        }
        const u32 levels = u32(down.size());
        for (u32 i = 0; i < levels; ++i) {
            const rhi::RGTexture in = i == 0 ? color : down[i - 1];
            const Extent2D inSize = i == 0 ? src : sizes[i - 1];
            const rhi::RGTexture out = down[i];
            const Extent2D outSize = sizes[i];
            const rhi::PipelineHandle pipe = m_down;
            g.addPass(std::format("Bloom.Down{}", i), rhi::PassType::Compute)
                .read(in, rhi::Access::SampledCompute)
                .overwrite(out, rhi::Access::StorageWriteCompute)
                .execute([=](rhi::PassContext& p) {
                    struct {
                        u32 src, dst, karis, pad;
                        glm::vec2 srcTexel, dstSize;
                    } pc{p.sampledIndex(in), p.storageIndex(out), i == 0 ? 1u : 0u, 0,
                         {1.0f / f32(inSize.width), 1.0f / f32(inSize.height)},
                         {f32(outSize.width), f32(outSize.height)}};
                    p.cmd.bindPipeline(pipe);
                    p.cmd.pushConstants(pc);
                    p.cmd.dispatchThreads(outSize.width, outSize.height);
                });
        }
        rhi::RGTexture low = down[levels - 1];
        for (i32 i = i32(levels) - 2; i >= 0; --i) {
            const rhi::RGTexture cur = down[usize(i)];
            const rhi::RGTexture lowTex = low;
            const Extent2D lowSize = sizes[usize(i) + 1];
            const Extent2D outSize = sizes[usize(i)];
            const rhi::RGTexture out = g.createTexture(texDesc(formats::kSceneColor, outSize.width, outSize.height, "Bloom.Up"));
            const f32 scale = i == 0 ? 1.0f / f32(levels) : 1.0f;
            const rhi::PipelineHandle pipe = m_up;
            g.addPass(std::format("Bloom.Up{}", i), rhi::PassType::Compute)
                .read(lowTex, rhi::Access::SampledCompute)
                .read(cur, rhi::Access::SampledCompute)
                .overwrite(out, rhi::Access::StorageWriteCompute)
                .execute([=](rhi::PassContext& p) {
                    struct {
                        u32 low, current, dst;
                        f32 scale;
                        glm::vec2 lowTexel, dstSize;
                    } pc{p.sampledIndex(lowTex), p.sampledIndex(cur), p.storageIndex(out), scale,
                         {1.0f / f32(lowSize.width), 1.0f / f32(lowSize.height)},
                         {f32(outSize.width), f32(outSize.height)}};
                    p.cmd.bindPipeline(pipe);
                    p.cmd.pushConstants(pc);
                    p.cmd.dispatchThreads(outSize.width, outSize.height);
                });
            low = out;
        }
        R.setTexture(kBloomResult, low);
    }

private:
    rhi::PipelineHandle m_down, m_up;
};

} // namespace

std::unique_ptr<IRenderFeature> makeBloomFeature() { return std::make_unique<BloomFeature>(); }

} // namespace ox::render::pp
