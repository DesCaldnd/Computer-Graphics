// Display-referred post on SceneColorLDR, first at the Overlay point (before editor overlays / UI): FXAA
// (r.AntiAliasing = FXAA), the user LUT texture of post-process volumes, film grain.
#include "pp_common.hpp"

#include <oxwald/render/gpu_resource_cache.hpp>
#include <oxwald/render/renderer.hpp>

namespace ox::render::pp {

namespace {

bool isSrgbFormat(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC7_SRGB_BLOCK: return true;
    default: return false;
    }
}

class LdrPostFeature final : public IRenderFeature {
public:
    std::string_view name() const override { return "LdrPost"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::Overlay); }
    i32 order() const override { return -100000; }
    std::vector<std::string_view> provides() const override { return {res::kSceneColorLDR}; }
    std::vector<std::string> cvarNames() const override { return {"r.AntiAliasing", "r.FXAA.Quality", "r.FilmGrain"}; }

    bool initialize(FeatureInitContext& ctx) override {
        m_pipeline = createComputePipeline(ctx.device, "pp.ldr", "render/postprocess/ldr_post.comp");
        return true;
    }
    void shutdown(rhi::Device& d) override { d.destroy(m_pipeline); }

    void setup(FeatureContext& ctx) override {
        const RenderSettings& rs = ctx.settings();
        const bool fxaa = rs.antiAliasing == 1 && !temporalUpscalerActive(rs, ctx.caps());
        const PostProcessSettings s = viewSettings(ctx);
        const f32 grain = cvFilmGrain.get() ? std::clamp(s.filmGrainIntensity, 0.0f, 1.0f) : 0.0f;
        u32 lutIndex = kInvalidIndex;
        f32 lutSize = 0.0f;
        bool lutSrgb = false;
        if (cvColorGrading.get() && s.lutTexture.isValid() && s.lutIntensity > 0.0f) {
            const GpuTexture* t = ctx.renderer().resources().texture(s.lutTexture);
            if (t && t->texture && !t->cube && t->height > 1 && t->width == t->height * t->height) {
                lutIndex = t->sampledIndex;
                lutSize = f32(t->height);
                lutSrgb = isSrgbFormat(ctx.device().desc(t->texture).format);
            }
        }
        if (!fxaa && grain <= 0.0f && lutIndex == kInvalidIndex) return;
        FrameResources& R = ctx.resources();
        const rhi::RGTexture src = R.texture(res::kSceneColorLDR);
        if (!src.valid()) return;
        const Extent2D size = ctx.outputExtent();
        const rhi::RGTexture out = ctx.graph().createTexture(texDesc(formats::kSceneColorLDR, size.width, size.height, "LdrPost"));
        static constexpr u32 kSteps[4] = {4, 8, 12, 16};
        const u32 steps = fxaa ? kSteps[std::clamp(cvFxaaQuality.get(), 0, 3)] : 0u;
        const f32 lutIntensity = std::clamp(s.lutIntensity, 0.0f, 1.0f);
        const u32 frame = u32(ctx.view().frameIndex());
        const rhi::PipelineHandle pipe = m_pipeline;
        ctx.graph()
            .addPass(fxaa ? "FXAA" : "LdrPost", rhi::PassType::Compute)
            .read(src, rhi::Access::SampledCompute)
            .overwrite(out, rhi::Access::StorageWriteCompute)
            .execute([=](rhi::PassContext& p) {
                struct {
                    u32 src, dst, fxaaSteps, lut;
                    f32 lutSize, lutIntensity;
                    u32 lutSrgb;
                    f32 grain;
                    u32 frame, pad;
                    glm::vec2 size;
                } pc{p.sampledIndex(src), p.storageIndex(out), steps, lutIndex, lutSize, lutIntensity,
                     lutSrgb ? 1u : 0u, grain, frame, 0, {f32(size.width), f32(size.height)}};
                p.cmd.bindPipeline(pipe);
                p.cmd.pushConstants(pc);
                p.cmd.dispatchThreads(size.width, size.height);
            });
        R.setTexture(res::kSceneColorLDR, out);
    }

private:
    rhi::PipelineHandle m_pipeline;
};

} // namespace

std::unique_ptr<IRenderFeature> makeLdrPostFeature() { return std::make_unique<LdrPostFeature>(); }

} // namespace ox::render::pp
