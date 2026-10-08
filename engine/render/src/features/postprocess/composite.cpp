// Output-resolution HDR post (AfterUpscale): CAS sharpening (order 50) and the composite (order 100) — chromatic
// aberration, bloom + lens dirt, vignette, white balance / colour grading through a 32³ log-space LUT baked on the
// GPU whenever the grading parameters change. Grading runs before the core tonemapper on exposed scene values, so
// every tonemapper (ACES/AgX/Neutral) sees the graded image; display-referred user LUTs run after tonemapping
// (ldr_post.cpp).
#include "pp_common.hpp"

#include <oxwald/core/hash.hpp>
#include <oxwald/render/gpu_resource_cache.hpp>
#include <oxwald/render/renderer.hpp>

#include <cmath>
#include <cstring>

namespace ox::render::pp {

namespace {

constexpr u32 kLutSize = 32;

glm::vec2 planckianUv(f32 t) {
    const f32 u = (0.860117757f + 1.54118254e-4f * t + 1.28641212e-7f * t * t) /
                  (1.0f + 8.42420235e-4f * t + 7.08145163e-7f * t * t);
    const f32 v = (0.317398726f + 4.22806245e-5f * t + 4.20481691e-8f * t * t) /
                  (1.0f - 2.89741816e-5f * t + 1.61456053e-7f * t * t);
    return {u, v};
}
glm::vec2 uvToXy(glm::vec2 uv) {
    const f32 d = 2.0f * uv.x - 8.0f * uv.y + 4.0f;
    return {3.0f * uv.x / d, 2.0f * uv.y / d};
}
// CIE daylight locus (4000 K … 25000 K).
glm::vec2 daylightXy(f32 t) {
    t *= 1.4388f / 1.438f;
    const f32 x = t <= 7000.0f ? 0.244063f + (0.09911e3f + (2.9678e6f - 4.6070e9f / t) / t) / t
                               : 0.237040f + (0.24748e3f + (1.9018e6f - 2.0064e9f / t) / t) / t;
    return {x, -3.0f * x * x + 2.87f * x - 0.275f};
}
// White point of an illuminant with colour temperature `t` and tint (offset along the isotherm, green −, magenta +).
glm::vec2 illuminantXy(f32 t, f32 tint) {
    t = std::clamp(t, 1000.0f, 40000.0f);
    const glm::vec2 planck = planckianUv(t);
    const glm::vec2 base = t < 4000.0f ? uvToXy(planck) : daylightXy(t);
    const f32 ud = (-1.13758118e9f - 1.91615621e6f * t - 1.53177f * t * t) /
                   std::pow(1.41213984e6f + 1189.62f * t + t * t, 2.0f);
    const f32 vd = (1.97471536e9f - 705674.0f * t - 308.607f * t * t) / std::pow(6.19363586e6f - 179.456f * t + t * t, 2.0f);
    const glm::vec2 dir = glm::normalize(glm::vec2(ud, vd));
    const glm::vec2 shifted = planck + glm::vec2(-dir.y, dir.x) * tint * 0.05f;
    return base + (uvToXy(shifted) - uvToXy(planck));
}
glm::vec3 xyToXYZ(glm::vec2 xy) { return {xy.x / xy.y, 1.0f, (1.0f - xy.x - xy.y) / xy.y}; }

// Rec.709 linear → Rec.709 linear: von Kries adaptation (CAT02) of the illuminant white to D65.
glm::mat3 adaptationMatrix(f32 temperature, f32 tint) {
    // glm matrices are column major: these are written as columns.
    const glm::mat3 rgbToXyz(0.4124564f, 0.2126729f, 0.0193339f, 0.3575761f, 0.7151522f, 0.1191920f, 0.1804375f,
                             0.0721750f, 0.9503041f);
    const glm::mat3 cat02(0.7328f, -0.7036f, 0.0030f, 0.4296f, 1.6975f, 0.0136f, -0.1624f, 0.0061f, 0.9834f);
    const glm::vec3 src = cat02 * xyToXYZ(illuminantXy(temperature, tint));
    const glm::vec3 dst = cat02 * xyToXYZ({0.31270f, 0.32900f});
    const glm::mat3 scale(glm::vec3(dst.x / src.x, 0, 0), glm::vec3(0, dst.y / src.y, 0), glm::vec3(0, 0, dst.z / src.z));
    return glm::inverse(rgbToXyz) * glm::inverse(cat02) * scale * cat02 * rgbToXyz;
}

bool gradingNeutral(const PostProcessSettings& s) {
    auto close = [](f32 a, f32 b) { return std::abs(a - b) < 1e-4f; };
    auto nearV = [&](glm::vec3 a, f32 b) { return close(a.x, b) && close(a.y, b) && close(a.z, b); };
    return std::abs(s.temperature - 6500.0f) < 0.5f && close(s.tint, 0.0f) && close(s.saturation, 1.0f) &&
           close(s.contrast, 1.0f) && nearV(s.lift, 0.0f) && nearV(s.gamma, 1.0f) && nearV(s.gain, 1.0f);
}

struct GradingViewState final : IFeatureViewState {
    rhi::TextureHandle lut;
    u64 hash = 0;
    void release(rhi::Device& d) override {
        if (lut) d.destroy(lut);
        lut = {};
    }
};

class CompositeFeature final : public IRenderFeature {
public:
    std::string_view name() const override { return "PostComposite"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::AfterUpscale); }
    i32 order() const override { return 100; }
    std::vector<std::string_view> provides() const override { return {res::kSceneColorHDR}; }
    std::vector<std::string> cvarNames() const override {
        return {"r.Bloom", "r.Vignette", "r.ChromaticAberration", "r.ColorGrading"};
    }

    bool initialize(FeatureInitContext& ctx) override {
        m_composite = createComputePipeline(ctx.device, "pp.composite", "render/postprocess/composite.comp");
        m_lut = createComputePipeline(ctx.device, "pp.grading.lut", "render/postprocess/grading_lut.comp");
        return true;
    }
    void shutdown(rhi::Device& d) override {
        d.destroy(m_composite);
        d.destroy(m_lut);
    }

    void setup(FeatureContext& ctx) override {
        const PostProcessSettings s = viewSettings(ctx);
        FrameResources& R = ctx.resources();
        const rhi::RGTexture src = R.texture(res::kSceneColorHDR);
        if (!src.valid()) return;
        const rhi::RGTexture bloom = cvBloom.get() && s.bloomIntensity > 0.0f ? R.texture(kBloomResult) : rhi::RGTexture{};
        const f32 vignette = cvVignette.get() ? std::clamp(s.vignetteIntensity, 0.0f, 1.0f) : 0.0f;
        const f32 chromatic = cvChromaticAberration.get() ? std::clamp(s.chromaticAberration, 0.0f, 1.0f) : 0.0f;
        const bool grading = cvColorGrading.get() && !gradingNeutral(s);
        if (!bloom.valid() && vignette <= 0.0f && chromatic <= 0.0f && !grading) return;

        rhi::Device& dev = ctx.device();
        rhi::RenderGraph& g = ctx.graph();
        u32 dirtIndex = kInvalidIndex;
        if (bloom.valid() && s.bloomDirtTexture.isValid() && s.bloomDirtIntensity > 0.0f) {
            if (const GpuTexture* t = ctx.renderer().resources().texture(s.bloomDirtTexture); t && t->texture && !t->cube) {
                dirtIndex = t->sampledIndex;
            }
        }

        rhi::RGTexture lutTex;
        if (grading) {
            GradingViewState& vs = ctx.viewState<GradingViewState>();
            if (!vs.lut) {
                rhi::TextureDesc d;
                d.type = rhi::TextureType::Tex3D;
                d.format = formats::kSceneColor;
                d.width = d.height = d.depth = kLutSize;
                d.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage;
                d.name = "pp.gradingLut";
                vs.lut = dev.createTexture(d);
                vs.hash = 0;
            }
            struct LutPush {
                u32 dst, size;
                glm::mat3 whiteBalance;
                f32 saturation, contrast;
                glm::vec3 lift, gamma, gain;
            } lp{};
            lp.size = kLutSize;
            lp.whiteBalance = adaptationMatrix(s.temperature, s.tint) * glm::inverse(adaptationMatrix(6500.0f, 0.0f));
            lp.saturation = std::max(s.saturation, 0.0f);
            lp.contrast = std::max(s.contrast, 0.0f);
            lp.lift = s.lift;
            lp.gamma = s.gamma;
            lp.gain = s.gain;
            static_assert(sizeof(LutPush) <= 128);
            const u64 h = fnv1a64(std::string_view(reinterpret_cast<const char*>(&lp), sizeof(lp))) | 1ull;
            lutTex = g.importTexture(dev, vs.lut);
            if (h != vs.hash) {
                vs.hash = h;
                const rhi::PipelineHandle pipe = m_lut;
                g.addPass("Grading.LUT", rhi::PassType::Compute)
                    .overwrite(lutTex, rhi::Access::StorageWriteCompute)
                    .execute([=](rhi::PassContext& p) {
                        LutPush pc = lp;
                        pc.dst = p.storageIndex(lutTex);
                        p.cmd.bindPipeline(pipe);
                        p.cmd.pushConstants(pc);
                        p.cmd.dispatch(kLutSize / 4, kLutSize / 4, kLutSize / 4);
                    });
            }
        }

        const Extent2D size = ctx.outputExtent();
        const rhi::RGTexture exposure = R.texture(kExposure);
        const rhi::RGTexture out = g.createTexture(texDesc(formats::kSceneColor, size.width, size.height, "PostComposite"));
        const VkDeviceAddress viewAddr = ctx.viewAddress();
        const f32 bloomIntensity = std::clamp(s.bloomIntensity, 0.0f, 1.0f);
        const f32 dirtIntensity = std::max(s.bloomDirtIntensity, 0.0f);
        const rhi::PipelineHandle pipe = m_composite;
        rhi::PassBuilder pb = g.addPass("PostComposite", rhi::PassType::Compute);
        pb.read(src, rhi::Access::SampledCompute).overwrite(out, rhi::Access::StorageWriteCompute);
        if (bloom.valid()) pb.read(bloom, rhi::Access::SampledCompute);
        if (lutTex.valid()) pb.read(lutTex, rhi::Access::SampledCompute);
        if (exposure.valid()) pb.read(exposure, rhi::Access::SampledCompute);
        pb.execute([=](rhi::PassContext& p) {
            struct {
                u64 view;
                u32 src, bloom, dirt, lut, exposureTex, dst;
                f32 bloomIntensity, dirtIntensity, vignette, chromatic, lutSize;
                u32 pad;
                glm::vec2 size;
            } pc{viewAddr,
                 p.sampledIndex(src),
                 bloom.valid() ? p.sampledIndex(bloom) : kInvalidIndex,
                 dirtIndex,
                 lutTex.valid() ? p.sampledIndex(lutTex) : kInvalidIndex,
                 exposure.valid() ? p.sampledIndex(exposure) : kInvalidIndex,
                 p.storageIndex(out),
                 bloomIntensity,
                 dirtIntensity,
                 vignette,
                 chromatic,
                 f32(kLutSize),
                 0,
                 {f32(size.width), f32(size.height)}};
            p.cmd.bindPipeline(pipe);
            p.cmd.pushConstants(pc);
            p.cmd.dispatchThreads(size.width, size.height);
        });
        R.setTexture(res::kSceneColorHDR, out);
    }

private:
    rhi::PipelineHandle m_composite, m_lut;
};

// r.Sharpen / volume `sharpen`: CAS on the output-resolution HDR image.
class SharpenFeature final : public IRenderFeature {
public:
    std::string_view name() const override { return "Sharpen"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::AfterUpscale); }
    i32 order() const override { return 50; }
    std::vector<std::string> cvarNames() const override { return {"r.Sharpen"}; }
    bool initialize(FeatureInitContext& ctx) override {
        m_cas.init(ctx.device);
        return true;
    }
    void shutdown(rhi::Device& d) override { m_cas.shutdown(d); }
    void setup(FeatureContext& ctx) override {
        const f32 amount = std::clamp(viewSettings(ctx).sharpen, 0.0f, 1.0f);
        if (amount <= 0.0f) return;
        FrameResources& R = ctx.resources();
        const rhi::RGTexture src = R.texture(res::kSceneColorHDR);
        if (!src.valid()) return;
        R.setTexture(res::kSceneColorHDR, m_cas.add(ctx, src, ctx.outputExtent(), amount, "Sharpen"));
    }

private:
    CasPass m_cas;
};

} // namespace

glm::mat3 whiteBalanceMatrix(f32 temperature, f32 tint) {
    return adaptationMatrix(temperature, tint) * glm::inverse(adaptationMatrix(6500.0f, 0.0f));
}

std::unique_ptr<IRenderFeature> makeCompositeFeature() { return std::make_unique<CompositeFeature>(); }
std::unique_ptr<IRenderFeature> makeSharpenFeature() { return std::make_unique<SharpenFeature>(); }

} // namespace ox::render::pp
