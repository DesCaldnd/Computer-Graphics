#pragma once

// NVIDIA DLSS (NGX) backend. Two implementations:
//   dlss_ngx.cpp  — the real NGX Vulkan integration; compiled into ox_render only when the vcpkg port provides the
//                   runtime (TARGET NVIDIA::DLSS: Windows/Linux) and OX_ENABLE_DLSS is on. On other platforms it is
//                   compiled (not linked) by the ox_dlss_syntax_check object library against the SDK headers.
//   dlss_stub.cpp — everywhere else: reports why DLSS is unavailable.
// Only the DLSS feature (dlss_feature.cpp) and the availability API (upscaler_api.cpp) talk to this interface.

#include <oxwald/render/features/postprocess/postprocess.hpp>
#include <oxwald/rhi/handles.hpp>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>

#include <string>
#include <vector>

namespace ox::rhi {
class Device;
class CommandList;
} // namespace ox::rhi

namespace ox::render::dlss {

// True when this binary contains the NGX implementation.
[[nodiscard]] bool compiled();
// Reason DLSS can never run in this build / on this platform (empty when compiled()).
[[nodiscard]] std::string buildUnavailableReason();

struct Status {
    bool available = false;
    std::string reason; // NGX error text, driver update request, GPU vendor... (empty when available)
    // DLSS Ray Reconstruction (NVSDK_NGX_Feature_RayReconstruction, "DLSS-D", runtime nvngx_dlssd.dll).
    bool rrAvailable = false;
    std::string rrReason;
};
// Initialises NGX for `device` on first call (once per device; NVSDK_NGX_VULKAN_Init_with_ProjectID) and queries the
// SuperSampling and SuperSamplingDenoising capability parameters. Cached afterwards. NGX is not called at all on
// non-NVIDIA GPUs and on devices created without requiredExtensions(). NGX stays initialised when either feature is
// available. Registers shutdown() as a shutdown callback of the device.
// Thread: render thread.
Status probe(rhi::Device& device);
// The current device was probed and DLSS cannot run on it (renderer falls back to TAAU without re-probing every
// frame). Forgotten when that device is destroyed.
[[nodiscard]] bool knownUnavailable();
// Same for Ray Reconstruction (the ray traced effects then keep their SVGF denoisers).
[[nodiscard]] bool rrKnownUnavailable();

// Instance/device extensions NGX requires (NVSDK_NGX_VULKAN_RequiredExtensions); empty without NGX.
void requiredExtensions(std::vector<std::string>& instanceExtensions, std::vector<std::string>& deviceExtensions);

struct OptimalSettings {
    bool valid = false;
    u32 renderWidth = 0, renderHeight = 0; // NGX_DLSS_GET_OPTIMAL_SETTINGS (dynamic range min/max ignored)
    f32 sharpness = 0.0f;
};
[[nodiscard]] OptimalSettings optimalSettings(rhi::Device& device, u32 outputWidth, u32 outputHeight,
                                              UpscalerQuality quality);

// One DLSS feature instance per view (NVSDK_NGX_Handle), recreated when sizes/quality/flags change.
struct ViewFeature {
    void* handle = nullptr; // NVSDK_NGX_Handle*
    u32 renderWidth = 0, renderHeight = 0, outputWidth = 0, outputHeight = 0;
    i32 quality = -1;
    i32 flags = 0;
};

struct EvaluateParams {
    rhi::TextureHandle color;    // render res, RGBA16F HDR, pre-exposed (sampled)
    rhi::TextureHandle depth;    // render res, D32 reversed-Z (sampled)
    rhi::TextureHandle motion;   // render res, RG16F uvCurrent - uvPrevious (sampled)
    rhi::TextureHandle exposure; // optional 1×1 R32F absolute exposure; invalid = NGX auto exposure
    rhi::TextureHandle output;   // output res, RGBA16F (storage, GENERAL)
    u32 renderWidth = 0, renderHeight = 0, outputWidth = 0, outputHeight = 0;
    glm::vec2 jitterPixels{0.0f}; // sub-pixel jitter of the projection in render pixels (x right, y down)
    f32 preExposure = 1.0f;
    f32 sharpness = 0.0f;
    f32 frameTimeMs = 16.6f;
    bool reset = false;
    UpscalerQuality quality = UpscalerQuality::Quality;
};
// Render resolution of Ray Reconstruction for an output size and quality mode (DLSSDOptimalSettingsCallback).
[[nodiscard]] OptimalSettings rrOptimalSettings(rhi::Device& device, u32 outputWidth, u32 outputHeight,
                                                UpscalerQuality quality);

// Inputs of a Ray Reconstruction evaluation (denoise + upscale of the noisy ray traced frame in one network).
struct RrEvaluateParams {
    rhi::TextureHandle color;           // render res, RGBA16F noisy HDR, pre-exposed
    rhi::TextureHandle depth;           // render res, D32 reversed-Z (hardware depth)
    rhi::TextureHandle motion;          // render res, RG16F uvCurrent - uvPrevious
    rhi::TextureHandle normals;         // render res, RGBA16F: xyz world normal, w perceptual roughness (packed)
    rhi::TextureHandle diffuseAlbedo;   // render res, RGBA8: baseColor × (1 − metallic)
    rhi::TextureHandle specularAlbedo;  // render res, RGBA8: pre-integrated GGX specular reflectance
    rhi::TextureHandle specularHitDistance; // optional, render res, r = reflection ray hit distance (world units)
    rhi::TextureHandle exposure;        // optional 1×1 R32F absolute exposure; invalid = NGX auto exposure
    rhi::TextureHandle output;          // output res, RGBA16F (storage, GENERAL)
    u32 renderWidth = 0, renderHeight = 0, outputWidth = 0, outputHeight = 0;
    glm::vec2 jitterPixels{0.0f};
    glm::mat4 worldToView{1.0f}; // column-major glm (= row-major with row vectors, the NGX convention)
    glm::mat4 viewToClip{1.0f};  // unjittered projection
    f32 preExposure = 1.0f;
    f32 frameTimeMs = 16.6f;
    bool reset = false;
    UpscalerQuality quality = UpscalerQuality::Native;
};
// Records a Ray Reconstruction evaluation (creates/recreates the per-view NVSDK_NGX_Feature_RayReconstruction feature
// when sizes / quality / flags change). Returns false (and logs once) when NGX fails.
bool evaluateRR(rhi::Device& device, rhi::CommandList& cmd, ViewFeature& feature, const RrEvaluateParams& params);

// Records the DLSS evaluation into `cmd` (creates/recreates the feature in the same command buffer when needed).
// Returns false (and logs once) when NGX fails; the caller then falls back for this frame.
bool evaluate(rhi::Device& device, rhi::CommandList& cmd, ViewFeature& feature, const EvaluateParams& params);
// Releases a view's feature (view destroyed / DLSS switched off). Waits nothing: call after the GPU is idle or
// from deferred destruction.
void release(ViewFeature& feature);
// NGX feature handles (DLSS super resolution + Ray Reconstruction, all views) created and not yet released.
[[nodiscard]] u32 liveFeatureCount();
// Shuts NGX down for this device and forgets the probe result. Every view feature must have been released. Called
// by the device's shutdown callback (rhi::Device::addShutdownCallback); renderers only release their features.
void shutdown(rhi::Device& device);

} // namespace ox::render::dlss
