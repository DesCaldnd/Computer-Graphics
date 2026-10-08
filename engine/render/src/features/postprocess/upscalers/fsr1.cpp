// AMD FidelityFX Super Resolution 1.0 in the Upscale slot: EASU (edge adaptive upsampling) + RCAS (robust contrast
// adaptive sharpening), GLSL compute ports (MIT). Spatial: it consumes the anti-aliased render-resolution image
// (TAA/FXAA at render resolution keep running) after post processing, in perceptual space.
#include "../pp_common.hpp"

#include <oxwald/render/render_settings.hpp>

namespace ox::render::pp {

namespace {

class Fsr1Feature final : public IRenderFeature {
public:
    std::string_view name() const override { return "FSR1"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::Upscale); }
    std::string_view exclusiveGroup() const override { return "Upscaler"; }
    i32 priority() const override { return 100; }
    std::vector<std::string_view> provides() const override { return {res::kSceneColorHDR}; }
    std::vector<std::string> cvarNames() const override {
        return {"r.Upscaler", "r.Upscaler.Quality", "r.Upscaler.Sharpness"};
    }
    bool isEnabled(const RenderSettings& s, const rhi::DeviceCaps& caps) const override {
        return effectiveUpscaler(s, caps) == UpscalerType::FSR1;
    }
    bool initialize(FeatureInitContext& ctx) override {
        m_easu = createComputePipeline(ctx.device, "pp.fsr1.easu", "render/postprocess/fsr1_easu.comp");
        m_rcas = createComputePipeline(ctx.device, "pp.fsr1.rcas", "render/postprocess/fsr1_rcas.comp");
        return true;
    }
    void shutdown(rhi::Device& d) override {
        d.destroy(m_easu);
        d.destroy(m_rcas);
    }
    void prepareView(ViewSetup& setup) override {
        const f32 scale = upscalerRenderScale(UpscalerQuality(std::clamp(setup.settings.upscalerQuality, 0, 4)));
        setup.screenPercentage = scale * 100.0f;
        setup.mipBias += upscalerMipBias(UpscalerType::FSR1, scale);
    }

    void setup(FeatureContext& ctx) override {
        FrameResources& R = ctx.resources();
        const rhi::RGTexture src = R.texture(res::kSceneColorHDR);
        if (!src.valid()) return;
        const Extent2D in = ctx.renderExtent(), out = ctx.outputExtent();
        const f32 sharpness = std::clamp(cvUpscalerSharpness.get(), 0.0f, 1.0f);
        const bool rcas = sharpness > 0.0f;
        rhi::RenderGraph& g = ctx.graph();
        const rhi::RGTexture easu = g.createTexture(texDesc(formats::kSceneColor, out.width, out.height, "FSR1.EASU"));
        const rhi::PipelineHandle pEasu = m_easu, pRcas = m_rcas;
        g.addPass("FSR1.EASU", rhi::PassType::Compute)
            .read(src, rhi::Access::SampledCompute)
            .overwrite(easu, rhi::Access::StorageWriteCompute)
            .execute([=](rhi::PassContext& p) {
                struct {
                    u32 src, dst, linearOut, pad;
                    glm::vec4 inSize, outSize;
                } pc{p.sampledIndex(src), p.storageIndex(easu), rcas ? 0u : 1u, 0,
                     {f32(in.width), f32(in.height), 1.0f / f32(in.width), 1.0f / f32(in.height)},
                     {f32(out.width), f32(out.height), 1.0f / f32(out.width), 1.0f / f32(out.height)}};
                p.cmd.bindPipeline(pEasu);
                p.cmd.pushConstants(pc);
                p.cmd.dispatchThreads(out.width, out.height);
            });
        if (!rcas) {
            R.setTexture(res::kSceneColorHDR, easu);
            return;
        }
        const rhi::RGTexture result = g.createTexture(texDesc(formats::kSceneColor, out.width, out.height, "FSR1.RCAS"));
        // r.Upscaler.Sharpness 1 = RCAS at full strength (0 stops), 0.5 = 1 stop, ...
        const f32 stops = (1.0f - sharpness) * 2.0f;
        g.addPass("FSR1.RCAS", rhi::PassType::Compute)
            .read(easu, rhi::Access::SampledCompute)
            .overwrite(result, rhi::Access::StorageWriteCompute)
            .execute([=](rhi::PassContext& p) {
                struct {
                    u32 src, dst;
                    f32 stops;
                    u32 pad;
                    glm::vec2 size;
                } pc{p.sampledIndex(easu), p.storageIndex(result), stops, 0, {f32(out.width), f32(out.height)}};
                p.cmd.bindPipeline(pRcas);
                p.cmd.pushConstants(pc);
                p.cmd.dispatchThreads(out.width, out.height);
            });
        R.setTexture(res::kSceneColorHDR, result);
    }

private:
    rhi::PipelineHandle m_easu, m_rcas;
};

} // namespace

std::unique_ptr<IRenderFeature> makeFsr1Feature() { return std::make_unique<Fsr1Feature>(); }

} // namespace ox::render::pp
