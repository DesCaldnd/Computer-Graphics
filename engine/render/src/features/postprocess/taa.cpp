// TAA (render resolution, r.AntiAliasing = TAA) and TAAU (temporal upsampling, r.Upscaler = TAAU, also the DLSS
// fallback). Both run at BeforePostProcess on the jittered frame; TAAU outputs the output resolution, so post
// processing runs at output resolution and the Upscale slot is reserved (no bilinear resample).
#include "pp_common.hpp"

#include <oxwald/render/render_settings.hpp>

namespace ox::render::pp {

namespace {

struct TaaViewState final : IFeatureViewState {
    f32 previousPreExposure = 0.0f;
};

class TemporalAAFeature final : public IRenderFeature {
public:
    explicit TemporalAAFeature(bool upsample) : m_upsample(upsample) {}

    std::string_view name() const override { return m_upsample ? "TAAU" : "TAA"; }
    InjectionMask injectionPoints() const override {
        return m_upsample ? maskOf(InjectionPoint::BeforePostProcess, InjectionPoint::Upscale)
                          : maskOf(InjectionPoint::BeforePostProcess);
    }
    std::string_view exclusiveGroup() const override { return m_upsample ? "Upscaler" : ""; }
    i32 priority() const override { return 200; }
    std::vector<std::string_view> provides() const override { return {res::kSceneColorHDR}; }
    std::vector<std::string> cvarNames() const override {
        std::vector<std::string> v{"r.TAA.Quality", "r.TAA.CurrentFrameWeight", "r.TAA.AntiFlicker"};
        if (m_upsample) {
            v.insert(v.end(), {"r.Upscaler", "r.Upscaler.Quality", "r.Upscaler.Sharpness"});
        } else {
            v.insert(v.end(), {"r.AntiAliasing", "r.AntiAliasing.Samples", "r.TAA.Sharpness"});
        }
        return v;
    }
    bool isEnabled(const RenderSettings& s, const rhi::DeviceCaps& caps) const override {
        if (m_upsample) return effectiveUpscaler(s, caps) == UpscalerType::TAAU;
        return s.antiAliasing == 2 && !temporalUpscalerActive(s, caps);
    }

    bool initialize(FeatureInitContext& ctx) override {
        m_pipeline = createComputePipeline(ctx.device, m_upsample ? "pp.taau" : "pp.taa", "render/postprocess/taa.comp");
        m_cas.init(ctx.device);
        return true;
    }
    void shutdown(rhi::Device& d) override {
        d.destroy(m_pipeline);
        m_cas.shutdown(d);
    }

    void prepareView(ViewSetup& setup) override {
        if (!m_upsample) {
            setup.jitterPhases = u32(std::clamp(cvTaaSamples.get(), 2, 64));
            return;
        }
        const f32 scale = upscalerRenderScale(UpscalerQuality(std::clamp(setup.settings.upscalerQuality, 0, 4)));
        setup.screenPercentage = scale * 100.0f;
        setup.jitterPhases = upscalerJitterPhases(scale);
        setup.mipBias += upscalerMipBias(UpscalerType::TAAU, scale);
    }

    void setup(FeatureContext& ctx) override {
        if (ctx.point() == InjectionPoint::Upscale) return; // already at output resolution
        FrameResources& R = ctx.resources();
        const rhi::RGTexture color = R.texture(res::kSceneColorHDR);
        const rhi::RGTexture depth = R.texture(res::kDepth);
        const rhi::RGTexture velocity = R.texture(res::kVelocity);
        if (!color.valid() || !depth.valid() || !velocity.valid()) return;
        const Extent2D in = ctx.renderExtent();
        const Extent2D out = m_upsample ? ctx.outputExtent() : in;

        rhi::TextureDesc hd = texDesc(formats::kSceneColor, out.width, out.height, "TAA.History");
        hd.usage = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled;
        const HistoryTexture hist = ctx.history(m_upsample ? "TAAU.Color" : "TAA.Color", hd);
        rhi::TextureDesc vd = texDesc(formats::kVelocity, out.width, out.height, "TAA.HistoryVelocity");
        vd.usage = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled;
        const HistoryTexture histVel = ctx.history(m_upsample ? "TAAU.Velocity" : "TAA.Velocity", vd);
        const rhi::RGTexture result =
            ctx.graph().createTexture(texDesc(formats::kSceneColor, out.width, out.height, m_upsample ? "TAAU.Output" : "TAA.Output"));

        TaaViewState& vs = ctx.viewState<TaaViewState>();
        const f32 pre = ctx.viewConstants().preExposure;
        const f32 exposureScale = vs.previousPreExposure > 0.0f ? pre / vs.previousPreExposure : 1.0f;
        vs.previousPreExposure = pre;

        const u32 quality = u32(std::clamp(cvTaaQuality.get(), 0, 3));
        const u32 flags = (hist.previousValid && histVel.previousValid ? 1u : 0u) | (cvTaaAntiFlicker.get() ? 2u : 0u) |
                          (quality << 8);
        const f32 currentWeight = std::clamp(cvTaaCurrentWeight.get(), 0.01f, 1.0f);
        const glm::vec2 jitter = ctx.view().jitterPixels();
        const VkDeviceAddress viewAddr = ctx.viewAddress();
        const rhi::PipelineHandle pipe = m_pipeline;
        ctx.graph()
            .addPass(m_upsample ? "TAAU" : "TAA", rhi::PassType::Compute)
            .read(color, rhi::Access::SampledCompute)
            .read(depth, rhi::Access::SampledCompute)
            .read(velocity, rhi::Access::SampledCompute)
            .read(hist.previous, rhi::Access::SampledCompute)
            .read(histVel.previous, rhi::Access::SampledCompute)
            .overwrite(hist.current, rhi::Access::StorageWriteCompute)
            .overwrite(histVel.current, rhi::Access::StorageWriteCompute)
            .overwrite(result, rhi::Access::StorageWriteCompute)
            .execute([=](rhi::PassContext& p) {
                struct {
                    u64 view;
                    u32 color, depth, velocity, historyColor, historyVel, outHistory, outHistoryVel, outColor, flags;
                    f32 exposureScale, currentWeight;
                    glm::vec4 inSize, outSize;
                    glm::vec2 jitter;
                } pc{viewAddr,
                     p.sampledIndex(color),
                     p.sampledIndex(depth),
                     p.sampledIndex(velocity),
                     p.sampledIndex(hist.previous),
                     p.sampledIndex(histVel.previous),
                     p.storageIndex(hist.current),
                     p.storageIndex(histVel.current),
                     p.storageIndex(result),
                     flags,
                     exposureScale,
                     currentWeight,
                     {f32(in.width), f32(in.height), 1.0f / f32(in.width), 1.0f / f32(in.height)},
                     {f32(out.width), f32(out.height), 1.0f / f32(out.width), 1.0f / f32(out.height)},
                     jitter};
                static_assert(sizeof(pc) <= 128);
                p.cmd.bindPipeline(pipe);
                p.cmd.pushConstants(pc);
                p.cmd.dispatchThreads(out.width, out.height);
            });
        rhi::RGTexture outTex = result;
        const f32 sharp = m_upsample ? cvUpscalerSharpness.get() : cvTaaSharpness.get();
        if (sharp > 0.0f) outTex = m_cas.add(ctx, result, out, sharp, m_upsample ? "TAAU.Sharpen" : "TAA.Sharpen");
        R.setTexture(res::kSceneColorHDR, outTex);
    }

private:
    bool m_upsample;
    rhi::PipelineHandle m_pipeline;
    CasPass m_cas;
};

} // namespace

std::unique_ptr<IRenderFeature> makeTaaFeature() { return std::make_unique<TemporalAAFeature>(false); }
std::unique_ptr<IRenderFeature> makeTaauFeature() { return std::make_unique<TemporalAAFeature>(true); }

} // namespace ox::render::pp
