// "AmbientOcclusion" feature: GTAO (or SSAO on Low) → spatial bilateral denoise → temporal accumulation →
// depth-aware upsample into AO (R8, render resolution). The lighting pass multiplies indirect diffuse by AO and
// derives the specular occlusion term from it (oxSpecularOcclusion, Lagarde 2014).
#include "reflection_internal.hpp"

namespace ox::render::reflections {

namespace {

constexpr VkFormat kR16F = VK_FORMAT_R16_SFLOAT;
constexpr VkFormat kHist = VK_FORMAT_R16G16B16A16_SFLOAT;

class AmbientOcclusionFeature final : public IRenderFeature {
public:
    std::string_view name() const override { return kAmbientOcclusionFeature; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::Lighting); }
    i32 order() const override { return -100; }
    std::string_view exclusiveGroup() const override { return "AO"; }
    std::vector<std::string_view> provides() const override { return {render::res::kAO}; }
    std::vector<std::string> cvarNames() const override { return cv::aoNames(); }
    bool isEnabled(const RenderSettings&, const rhi::DeviceCaps&) const override { return cv::aoMethod.get() > 0; }

    bool initialize(FeatureInitContext& ctx) override {
        rhi::Device& dev = ctx.device;
        m_gtao = createComputePipeline(dev, "render.ao.gtao", "render/reflections/gtao.comp", {{"OX_AO_GTAO"}});
        m_ssao = createComputePipeline(dev, "render.ao.ssao", "render/reflections/gtao.comp");
        m_denoise = createComputePipeline(dev, "render.ao.denoise", "render/reflections/ao_denoise.comp");
        m_temporal = createComputePipeline(dev, "render.ao.temporal", "render/reflections/ao_temporal.comp");
        m_upsample = createComputePipeline(dev, "render.ao.upsample", "render/reflections/ao_upsample.comp");
        return true;
    }
    void shutdown(rhi::Device& dev) override {
        for (rhi::PipelineHandle p : {m_gtao, m_ssao, m_denoise, m_temporal, m_upsample}) {
            if (p) dev.destroy(p);
        }
    }

    void setup(FeatureContext& ctx) override {
        FrameResources& R = ctx.resources();
        rhi::RenderGraph& g = ctx.graph();
        const Extent2D re = ctx.renderExtent();
        const u32 ds = cv::aoHalfRes ? 2u : 1u;
        const Extent2D ae{(re.width + ds - 1) / ds, (re.height + ds - 1) / ds};
        const rhi::RGTexture depth = R.texture(render::res::kDepth), normals = R.texture(render::res::kNormals);
        const rhi::RGTexture velocity = R.texture(render::res::kVelocity);
        const VkDeviceAddress viewAddr = ctx.viewAddress(), sceneAddr = ctx.sceneAddress();

        const u32 method = u32(std::clamp(cv::aoMethod.get(), 1, 2));
        const u32 q = u32(std::clamp(cv::aoQuality.get(), 0, 3));
        static constexpr u32 kSlices[4] = {1, 2, 2, 3};
        static constexpr u32 kSteps[4] = {4, 6, 8, 12};
        static constexpr u32 kSsaoSamples[4] = {8, 12, 16, 24};
        const u32 slices = kSlices[q];
        const u32 steps = method == 2 ? kSteps[q] : kSsaoSamples[q];
        const f32 radius = cv::aoRadius.get();
        const rhi::PipelineHandle aoPipe = method == 2 ? m_gtao : m_ssao;

        const rhi::RGTexture raw = g.createTexture(textureDesc(kR16F, ae, "AO.Raw"));
        g.addPass("AO.Trace", rhi::PassType::Compute)
            .read(depth, rhi::Access::SampledCompute)
            .read(normals, rhi::Access::SampledCompute)
            .overwrite(raw, rhi::Access::StorageWriteCompute)
            .execute([=](rhi::PassContext& p) {
                struct {
                    u64 view, scene;
                    u32 depth, normals, out, downscale, slices, steps;
                    f32 radius, falloff;
                    u32 w, h;
                } pc{viewAddr, sceneAddr, p.sampledIndex(depth), p.sampledIndex(normals), p.storageIndex(raw), ds, slices,
                     steps, radius, 0.6f, ae.width, ae.height};
                p.cmd.bindPipeline(aoPipe);
                p.cmd.pushConstants(pc);
                p.cmd.dispatchThreads(ae.width, ae.height);
            });

        const rhi::RGTexture denoised = g.createTexture(textureDesc(kR16F, ae, "AO.Denoised"));
        const rhi::PipelineHandle denoise = m_denoise;
        g.addPass("AO.Denoise", rhi::PassType::Compute)
            .read(raw, rhi::Access::SampledCompute)
            .read(depth, rhi::Access::SampledCompute)
            .read(normals, rhi::Access::SampledCompute)
            .overwrite(denoised, rhi::Access::StorageWriteCompute)
            .execute([=](rhi::PassContext& p) {
                struct {
                    u64 view, scene;
                    u32 src, depth, normals, dst, downscale, w, h;
                } pc{viewAddr, sceneAddr, p.sampledIndex(raw), p.sampledIndex(depth), p.sampledIndex(normals),
                     p.storageIndex(denoised), ds, ae.width, ae.height};
                p.cmd.bindPipeline(denoise);
                p.cmd.pushConstants(pc);
                p.cmd.dispatchThreads(ae.width, ae.height);
            });

        rhi::TextureDesc hd = textureDesc(kHist, ae, "AO.History");
        hd.usage = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled;
        const HistoryTexture hist = ctx.history("AO.History", hd);
        const u32 valid = cv::aoTemporal && hist.previousValid ? 1u : 0u;
        const f32 blend = q >= 2 ? 0.1f : 0.15f;
        const rhi::PipelineHandle temporal = m_temporal;
        g.addPass("AO.Temporal", rhi::PassType::Compute)
            .read(denoised, rhi::Access::SampledCompute)
            .read(hist.previous, rhi::Access::SampledCompute)
            .read(depth, rhi::Access::SampledCompute)
            .read(velocity, rhi::Access::SampledCompute)
            .overwrite(hist.current, rhi::Access::StorageWriteCompute)
            .execute([=](rhi::PassContext& p) {
                struct {
                    u64 view, scene;
                    u32 current, history, depth, velocity, dst, downscale, w, h, valid;
                    f32 blend;
                } pc{viewAddr, sceneAddr, p.sampledIndex(denoised), p.sampledIndex(hist.previous), p.sampledIndex(depth),
                     p.sampledIndex(velocity), p.storageIndex(hist.current), ds, ae.width, ae.height, valid, blend};
                p.cmd.bindPipeline(temporal);
                p.cmd.pushConstants(pc);
                p.cmd.dispatchThreads(ae.width, ae.height);
            });

        const rhi::RGTexture ao = g.createTexture(textureDesc(formats::kAO, re, "AO"));
        const f32 power = std::max(cv::aoIntensity.get(), 0.0f);
        const rhi::PipelineHandle upsample = m_upsample;
        const rhi::RGTexture accumulated = hist.current;
        g.addPass("AO.Upsample", rhi::PassType::Compute)
            .read(accumulated, rhi::Access::SampledCompute)
            .read(depth, rhi::Access::SampledCompute)
            .overwrite(ao, rhi::Access::StorageWriteCompute)
            .execute([=](rhi::PassContext& p) {
                struct {
                    u64 view, scene;
                    u32 src, depth, dst, downscale, w, h;
                    f32 power;
                } pc{viewAddr, sceneAddr, p.sampledIndex(accumulated), p.sampledIndex(depth), p.storageIndex(ao), ds,
                     ae.width, ae.height, power};
                p.cmd.bindPipeline(upsample);
                p.cmd.pushConstants(pc);
                p.cmd.dispatchThreads(re.width, re.height);
            });
        R.setTexture(render::res::kAO, ao);
    }

private:
    rhi::PipelineHandle m_gtao, m_ssao, m_denoise, m_temporal, m_upsample;
};

} // namespace

std::unique_ptr<IRenderFeature> makeAmbientOcclusionFeature() { return std::make_unique<AmbientOcclusionFeature>(); }

} // namespace ox::render::reflections
