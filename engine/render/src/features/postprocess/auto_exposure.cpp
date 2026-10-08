// Physically based auto exposure (eye adaptation): luminance histogram → percentile average → EV100 adaptation.
//
// PreDepth: feeds the pre-exposure hook — VIEW.preExposure of this frame is the exposure the GPU computed
// kReadbackLatency frames ago (read back through a small host-visible ring), so SceneColorHDR stays in a well
// conditioned FP16 range whatever the scene brightness. Temporal features rescale their history by
// preExposure(now) / preExposure(previous).
// BeforePostProcess (order -100, before TAA / DLSS): histogram + adaptation → `Exposure` (R32F 1×1, read by the
// tonemapper and DLSS).
#include "pp_common.hpp"

#include <oxwald/render/render_settings.hpp>

#include <cmath>

namespace ox::render::pp {

namespace {

constexpr u32 kBins = 128;
constexpr u32 kReadbackRing = 4;
constexpr u32 kReadbackLatency = 3; // frames in flight + 1

struct ExposureViewState final : IFeatureViewState {
    rhi::BufferHandle histogram; // u32[128], persistent (cleared by the adaptation pass)
    rhi::BufferHandle state;     // vec4
    rhi::BufferHandle readback;  // f32[kReadbackRing], host visible
    u64 slotFrame[kReadbackRing] = {~0ull, ~0ull, ~0ull, ~0ull};
    f32 lastPreExposure = 0.0f;
    bool everRan = false;

    void release(rhi::Device& device) override {
        for (auto b : {histogram, state, readback}) {
            if (b) device.destroy(b);
        }
        histogram = state = readback = {};
    }
};

class AutoExposureFeature final : public IRenderFeature {
public:
    std::string_view name() const override { return "AutoExposure"; }
    InjectionMask injectionPoints() const override {
        return maskOf(InjectionPoint::PreDepth, InjectionPoint::BeforePostProcess);
    }
    i32 order() const override { return -100; }
    std::vector<std::string_view> provides() const override { return {kExposure}; }
    std::vector<std::string> cvarNames() const override {
        return {"r.Exposure.Auto", "r.Exposure.MinEV100", "r.Exposure.MaxEV100", "r.Exposure.SpeedUp",
                "r.Exposure.SpeedDown"};
    }
    // Volumes may switch eye adaptation on, so the feature stays resolved and decides per view in setup().
    bool isEnabled(const RenderSettings& s, const rhi::DeviceCaps&) const override {
        return s.exposureMode != ExposureMode::Manual;
    }

    bool initialize(FeatureInitContext& ctx) override {
        m_histogram = createComputePipeline(ctx.device, "pp.exposure.histogram", "render/postprocess/exposure_histogram.comp");
        m_adapt = createComputePipeline(ctx.device, "pp.exposure.adapt", "render/postprocess/exposure_adapt.comp");
        return true;
    }
    void shutdown(rhi::Device& d) override {
        d.destroy(m_histogram);
        d.destroy(m_adapt);
    }

    void setup(FeatureContext& ctx) override {
        const PostProcessSettings pp = viewSettings(ctx);
        if (!pp.autoExposure) return;
        ExposureViewState& vs = ctx.viewState<ExposureViewState>();
        rhi::Device& dev = ctx.device();
        if (!vs.histogram) {
            const std::vector<u32> zeros(kBins, 0u);
            vs.histogram = dev.createBuffer({kBins * 4, rhi::BufferUsage::Storage, rhi::MemoryUsage::GpuOnly,
                                             "pp.exposure.histogram"},
                                            zeros.data());
            const glm::vec4 zero(0.0f);
            vs.state = dev.createBuffer({sizeof(glm::vec4), rhi::BufferUsage::Storage, rhi::MemoryUsage::GpuOnly,
                                         "pp.exposure.state"},
                                        &zero);
            vs.readback = dev.createBuffer({kReadbackRing * 4, rhi::BufferUsage::Storage, rhi::MemoryUsage::Readback,
                                            "pp.exposure.readback"});
        }
        const u64 frame = ctx.view().frameIndex();
        if (ctx.point() == InjectionPoint::PreDepth) {
            // Pre-exposure from the newest result that is certainly complete.
            if (frame >= kReadbackLatency) {
                const u32 slot = u32((frame - kReadbackLatency) % kReadbackRing);
                const auto* values = static_cast<const f32*>(dev.mapped(vs.readback));
                if (values && vs.slotFrame[slot] == frame - kReadbackLatency) {
                    const f32 e = values[slot];
                    if (std::isfinite(e) && e > 0.0f) ctx.viewConstants().preExposure = e;
                }
            }
            vs.lastPreExposure = ctx.viewConstants().preExposure;
            return;
        }

        FrameResources& R = ctx.resources();
        const rhi::RGTexture color = R.texture(res::kSceneColorHDR);
        if (!color.valid()) return;
        const rhi::RGTexture exposure = ctx.graph().createTexture(texDesc(VK_FORMAT_R32_SFLOAT, 1, 1, "Exposure"));
        const u32 slot = u32(frame % kReadbackRing);
        vs.slotFrame[slot] = frame;
        const RenderSettings& rs = ctx.settings();
        struct AdaptPush {
            u64 histogram, state, readback;
            u32 exposureTex, readbackSlot;
            f32 minEV, maxEV, compensation, speedUp, speedDown, deltaTime, lowFraction, highFraction;
            u32 reset;
        } ap{};
        ap.histogram = dev.address(vs.histogram);
        ap.state = dev.address(vs.state);
        ap.readback = dev.address(vs.readback);
        ap.readbackSlot = slot;
        ap.minEV = std::min(pp.minEV100, pp.maxEV100);
        ap.maxEV = std::max(pp.minEV100, pp.maxEV100);
        ap.compensation = pp.exposureCompensation + rs.exposureCompensation;
        ap.speedUp = pp.adaptationSpeedUp;
        ap.speedDown = pp.adaptationSpeedDown;
        const f32 dt = ctx.snapshot().deltaTime;
        ap.deltaTime = dt > 0.0f ? std::min(dt, 0.25f) : 1.0f / 60.0f;
        ap.lowFraction = std::clamp(pp.histogramLowPercent, 0.0f, 100.0f) / 100.0f;
        ap.highFraction = std::clamp(pp.histogramHighPercent, 0.0f, 100.0f) / 100.0f;
        ap.reset = vs.everRan ? 0u : 1u;
        vs.everRan = true;
        const Extent2D size = ctx.renderExtent();
        const VkDeviceAddress viewAddr = ctx.viewAddress();
        const rhi::PipelineHandle hist = m_histogram, adapt = m_adapt;
        ctx.graph()
            .addPass("AutoExposure", rhi::PassType::Compute)
            .read(color, rhi::Access::SampledCompute)
            .overwrite(exposure, rhi::Access::StorageWriteCompute)
            .execute([=](rhi::PassContext& p) {
                AdaptPush a = ap;
                a.exposureTex = p.storageIndex(exposure);
                // The persistent buffers were last written by the previous frame's adaptation.
                p.cmd.memoryBarrier(rhi::Access::StorageWriteCompute, rhi::Access::StorageWriteCompute);
                struct {
                    u64 view, histogram;
                    u32 color, pad;
                    glm::vec2 size;
                } hp{viewAddr, a.histogram, p.sampledIndex(color), 0, {f32(size.width), f32(size.height)}};
                p.cmd.bindPipeline(hist);
                p.cmd.pushConstants(hp);
                p.cmd.dispatch(divUp(divUp(size.width, 2), 16), divUp(divUp(size.height, 2), 16));
                p.cmd.memoryBarrier(rhi::Access::StorageWriteCompute, rhi::Access::StorageWriteCompute);
                p.cmd.bindPipeline(adapt);
                p.cmd.pushConstants(a);
                p.cmd.dispatch(1);
                p.cmd.memoryBarrier(rhi::Access::StorageWriteCompute, rhi::Access::HostRead);
            });
        R.setTexture(kExposure, exposure);
    }

private:
    rhi::PipelineHandle m_histogram, m_adapt;
};

} // namespace

std::unique_ptr<IRenderFeature> makeAutoExposureFeature() { return std::make_unique<AutoExposureFeature>(); }

} // namespace ox::render::pp
