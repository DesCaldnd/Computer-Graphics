// NVIDIA DLSS super resolution / DLAA as a temporal upscaler at BeforePostProcess (jittered render-resolution
// colour + depth + motion vectors + exposure → output resolution); it also reserves the Upscale slot. Requires an
// NVIDIA RTX GPU on Windows/Linux (dlss_ngx.cpp); everywhere else r.Upscaler=DLSS falls back to TAAU and the
// settings UI shows upscalerAvailability()'s reason.
#include "../pp_common.hpp"
#include "dlss_backend.hpp"

#include <oxwald/core/log.hpp>
#include <oxwald/render/render_settings.hpp>

#include <algorithm>
#include <map>
#include <tuple>

namespace ox::render::pp {

namespace {

class DlssFeature;

struct DlssViewState final : IFeatureViewState {
    dlss::ViewFeature feature;
    DlssFeature* owner = nullptr;
    void release(rhi::Device& device) override;
};

class DlssFeature final : public IRenderFeature {
public:
    std::string_view name() const override { return "DLSS"; }
    InjectionMask injectionPoints() const override {
        return maskOf(InjectionPoint::BeforePostProcess, InjectionPoint::Upscale);
    }
    std::string_view exclusiveGroup() const override { return "Upscaler"; }
    i32 priority() const override { return 300; }
    std::vector<std::string_view> provides() const override { return {res::kSceneColorHDR}; }
    std::vector<std::string> cvarNames() const override {
        return {"r.Upscaler", "r.Upscaler.Quality", "r.Upscaler.Sharpness"};
    }
    bool isEnabled(const RenderSettings& s, const rhi::DeviceCaps& caps) const override {
        return effectiveUpscaler(s, caps) == UpscalerType::DLSS;
    }

    bool initialize(FeatureInitContext& ctx) override {
        const dlss::Status st = dlss::probe(ctx.device);
        if (!st.available) {
            OX_LOG_WARN("render", "DLSS unavailable, falling back to TAAU: {}", st.reason);
            return false;
        }
        m_device = &ctx.device;
        m_fallback = createComputePipeline(ctx.device, "pp.dlss.fallback", "render/postprocess/fsr1_easu.comp");
        return true;
    }
    void shutdown(rhi::Device& d) override {
        d.waitIdle();
        for (DlssViewState* s : m_states) {
            dlss::release(s->feature);
            s->owner = nullptr;
        }
        m_states.clear();
        d.destroy(m_fallback);
        dlss::shutdown(d);
    }
    void forget(DlssViewState* s) { std::erase(m_states, s); }

    void prepareView(ViewSetup& setup) override {
        const UpscalerQuality q = UpscalerQuality(std::clamp(setup.settings.upscalerQuality, 0, 4));
        f32 scale = upscalerRenderScale(q);
        if (m_device && q != UpscalerQuality::Native) {
            const auto key = std::make_tuple(setup.outputExtent.width, setup.outputExtent.height, i32(q));
            auto it = m_optimal.find(key);
            if (it == m_optimal.end()) {
                it = m_optimal.emplace(key, dlss::optimalSettings(*m_device, setup.outputExtent.width,
                                                                  setup.outputExtent.height, q)).first;
            }
            if (it->second.valid && setup.outputExtent.width > 0) {
                scale = f32(it->second.renderWidth) / f32(setup.outputExtent.width);
            }
        }
        setup.screenPercentage = scale * 100.0f;
        setup.jitterPhases = upscalerJitterPhases(scale);
        setup.mipBias += upscalerMipBias(UpscalerType::DLSS, scale);
    }

    void setup(FeatureContext& ctx) override {
        if (ctx.point() == InjectionPoint::Upscale) return;
        FrameResources& R = ctx.resources();
        const rhi::RGTexture color = R.texture(res::kSceneColorHDR);
        const rhi::RGTexture depth = R.texture(res::kDepth);
        const rhi::RGTexture velocity = R.texture(res::kVelocity);
        if (!color.valid() || !depth.valid() || !velocity.valid()) return;
        const rhi::RGTexture exposure = R.texture(kExposure);
        DlssViewState& vs = ctx.viewState<DlssViewState>();
        if (!vs.owner) {
            vs.owner = this;
            m_states.push_back(&vs);
        }
        const Extent2D in = ctx.renderExtent(), out = ctx.outputExtent();
        const rhi::RGTexture result = ctx.graph().createTexture(texDesc(formats::kSceneColor, out.width, out.height, "DLSS.Output"));
        dlss::EvaluateParams ep;
        ep.renderWidth = in.width;
        ep.renderHeight = in.height;
        ep.outputWidth = out.width;
        ep.outputHeight = out.height;
        ep.jitterPixels = ctx.view().jitterPixels();
        ep.preExposure = ctx.viewConstants().preExposure;
        ep.sharpness = std::clamp(cvUpscalerSharpness.get(), 0.0f, 1.0f);
        ep.frameTimeMs = ctx.snapshot().deltaTime > 0.0f ? ctx.snapshot().deltaTime * 1000.0f : 16.6f;
        ep.reset = ctx.view().cameraCut();
        ep.quality = UpscalerQuality(std::clamp(ctx.settings().upscalerQuality, 0, 4));
        dlss::ViewFeature* feature = &vs.feature;
        rhi::Device* device = &ctx.device();
        const rhi::PipelineHandle fallback = m_fallback;
        rhi::PassBuilder pb = ctx.graph().addPass("DLSS", rhi::PassType::Compute);
        pb.read(color, rhi::Access::SampledCompute)
            .read(depth, rhi::Access::SampledCompute)
            .read(velocity, rhi::Access::SampledCompute)
            .overwrite(result, rhi::Access::StorageWriteCompute);
        if (exposure.valid()) pb.read(exposure, rhi::Access::SampledCompute);
        pb.execute([=](rhi::PassContext& p) {
            dlss::EvaluateParams e = ep;
            e.color = p.texture(color);
            e.depth = p.texture(depth);
            e.motion = p.texture(velocity);
            e.exposure = exposure.valid() ? p.texture(exposure) : rhi::TextureHandle{};
            e.output = p.texture(result);
            if (dlss::evaluate(*device, p.cmd, *feature, e)) return;
            // NGX failed this frame: spatial upscale so the frame is still complete.
            struct {
                u32 src, dst, linearOut, pad;
                glm::vec4 inSize, outSize;
            } pc{p.sampledIndex(color), p.storageIndex(result), 1u, 0,
                 {f32(in.width), f32(in.height), 1.0f / f32(in.width), 1.0f / f32(in.height)},
                 {f32(out.width), f32(out.height), 1.0f / f32(out.width), 1.0f / f32(out.height)}};
            p.cmd.bindPipeline(fallback);
            p.cmd.pushConstants(pc);
            p.cmd.dispatchThreads(out.width, out.height);
        });
        R.setTexture(res::kSceneColorHDR, result);
    }

private:
    rhi::Device* m_device = nullptr;
    rhi::PipelineHandle m_fallback;
    std::vector<DlssViewState*> m_states;
    std::map<std::tuple<u32, u32, i32>, dlss::OptimalSettings> m_optimal;
};

void DlssViewState::release(rhi::Device&) {
    if (owner) {
        dlss::release(feature);
        owner->forget(this);
        owner = nullptr;
    }
}

} // namespace

std::unique_ptr<IRenderFeature> makeDlssFeature() { return std::make_unique<DlssFeature>(); }

} // namespace ox::render::pp
