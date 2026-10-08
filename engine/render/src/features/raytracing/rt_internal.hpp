#pragma once

// Private state shared by the ray tracing features of one Renderer: settings snapshot, the shared denoiser, the
// per-frame scene header written by the RayTracingScene feature, plus a base class with the common plumbing of the
// ray traced effects.

#include "../../renderer_impl.hpp"

#include <oxwald/render/features/raytracing/ddgi.hpp>
#include <oxwald/render/features/raytracing/denoiser.hpp>
#include <oxwald/render/features/raytracing/raytracing.hpp>
#include <oxwald/render/features/raytracing/rt_api.hpp>

#include <memory>

namespace ox::render::rt {

// Mirrors RtSceneHeader in raytracing/rt_common.glsl.
struct RtSceneHeaderGpu {
    u64 tlas = 0;
    u64 instances = 0;
    u64 indices = 0;
    u32 instanceCount = 0;
    u32 frame = 0;
    DdgiVolumeGpu ddgi;
};
static_assert(sizeof(RtSceneHeaderGpu) == 128);

struct RtShared {
    RtSettings settings;
    u64 settingsFrame = ~0ull;

    SvgfDenoiser denoiser;
    u32 denoiserUsers = 0;

    // Per-frame scene (RayTracingScene feature, first view of the frame).
    u64 sceneFrame = ~0ull;
    bool sceneActive = false;
    VkDeviceAddress header = 0;
    RtSceneHeaderGpu* headerCpu = nullptr; // frame memory: patchable until the graph executes (DDGI)
    u64 tlasAddress = 0;
    u32 tlasInstances = 0;
    u64 sceneStructureHash = 0; // changes with instances / BLAS / materials (path tracer reset)

    const RtSettings& refresh(rhi::Device& device) {
        if (settingsFrame != device.frameNumber()) {
            settings = RtSettings::fromCVars();
            settingsFrame = device.frameNumber();
        }
        return settings;
    }
    bool acquireDenoiser(rhi::Device& device) {
        ++denoiserUsers;
        return denoiser.initialize(device);
    }
    void releaseDenoiser(rhi::Device& device) {
        if (denoiserUsers > 0 && --denoiserUsers == 0) denoiser.shutdown(device);
    }
    [[nodiscard]] bool sceneReady(rhi::Device& device) const { return sceneActive && sceneFrame == device.frameNumber(); }
};

// Common inputs of a ray traced screen-space pass.
struct RtViewInputs {
    rhi::RGTexture depth, normals, velocity;
    rhi::RGBuffer rtScene;
    VkDeviceAddress view = 0, scene = 0, rt = 0;
    Extent2D render;
    u32 frame = 0;
};

class RtFeatureBase : public IRenderFeature {
public:
    explicit RtFeatureBase(std::shared_ptr<RtShared> shared) : m_shared(std::move(shared)) {}

protected:
    // r.RayTracing + device support; the effect cvar is checked by the derived class.
    [[nodiscard]] static bool rtOn(const RenderSettings& s, const rhi::DeviceCaps& caps) { return rayTracingActive(s, caps); }
    [[nodiscard]] const RtSettings& rts() const { return m_shared->settings; }
    // Valid inputs only when the TLAS of this frame exists (else the effect publishes nothing this frame).
    [[nodiscard]] bool gather(FeatureContext& ctx, RtViewInputs& out) const {
        rhi::Device& dev = ctx.device();
        m_shared->refresh(dev);
        if (!m_shared->sceneReady(dev)) return false;
        FrameResources& R = ctx.resources();
        out.depth = R.texture(ox::render::res::kDepth);
        out.normals = R.texture(ox::render::res::kNormals);
        out.velocity = R.texture(ox::render::res::kVelocity);
        out.rtScene = R.buffer(res::kRtScene);
        if (!out.depth.valid() || !out.normals.valid() || !out.rtScene.valid()) return false;
        out.view = ctx.viewAddress();
        out.scene = ctx.sceneAddress();
        out.rt = m_shared->header;
        out.render = ctx.renderExtent();
        out.frame = u32(ctx.view().frameIndex());
        return true;
    }
    // Signal extent for a resolution scale in percent (50 = half resolution).
    [[nodiscard]] static Extent2D scaled(Extent2D e, i32 percent) {
        if (percent >= 100) return e;
        return {std::max(1u, (e.width + 1) / 2), std::max(1u, (e.height + 1) / 2)};
    }
    static rhi::TextureDesc texDesc(VkFormat format, Extent2D e, std::string name) {
        rhi::TextureDesc d;
        d.format = format;
        d.width = std::max(e.width, 1u);
        d.height = std::max(e.height, 1u);
        d.usage = rhi::TextureUsage::None;
        d.name = std::move(name);
        return d;
    }
    static DenoiserInputs denoiserInputs(const RtViewInputs& in, rhi::RGTexture signal, Extent2D signalExtent) {
        DenoiserInputs d;
        d.signal = signal;
        d.depth = in.depth;
        d.normals = in.normals;
        d.velocity = in.velocity;
        d.signalExtent = signalExtent;
        d.renderExtent = in.render;
        d.scale = signalExtent == in.render ? 1u : 2u;
        return d;
    }
    DenoiserSettings denoiserSettings(bool enabled, DenoiseOutput output, glm::vec4 luma) const {
        DenoiserSettings d;
        d.enabled = enabled;
        d.iterations = u32(std::max(rts().denoiserIterations, 0));
        d.maxHistory = u32(std::max(rts().denoiserMaxHistory, 1));
        d.output = output;
        d.lumaWeights = luma;
        return d;
    }

    std::shared_ptr<RtShared> m_shared;
};

inline u32 dispatchGroups(u32 n, u32 local = 8) { return (n + local - 1) / local; }

// Feature factories (one file each).
std::unique_ptr<IRenderFeature> makeRayTracingSceneFeature(std::shared_ptr<RtShared> shared);
std::unique_ptr<IRenderFeature> makeShadowsRtFeature(std::shared_ptr<RtShared> shared);
std::unique_ptr<IRenderFeature> makeReflectionsRtFeature(std::shared_ptr<RtShared> shared);
std::unique_ptr<IRenderFeature> makeAmbientOcclusionRtFeature(std::shared_ptr<RtShared> shared);
std::unique_ptr<IRenderFeature> makeGlobalIlluminationRtFeature(std::shared_ptr<RtShared> shared);
std::unique_ptr<IRenderFeature> makeTranslucencyRtFeature(std::shared_ptr<RtShared> shared);
std::unique_ptr<IRenderFeature> makeVolumetricsRtFeature(std::shared_ptr<RtShared> shared);
std::unique_ptr<IRenderFeature> makePathTracerFeature(std::shared_ptr<RtShared> shared);

} // namespace ox::render::rt
