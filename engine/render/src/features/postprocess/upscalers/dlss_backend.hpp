#pragma once

// NVIDIA DLSS (NGX) backend. Two implementations:
//   dlss_ngx.cpp  — the real NGX Vulkan integration; compiled into ox_render only when the vcpkg port provides the
//                   runtime (TARGET NVIDIA::DLSS: Windows/Linux) and OX_ENABLE_DLSS is on. On other platforms it is
//                   compiled (not linked) by the ox_dlss_syntax_check object library against the SDK headers.
//   dlss_stub.cpp — everywhere else: reports why DLSS is unavailable.
// Only the DLSS feature (dlss_feature.cpp) and the availability API (upscaler_api.cpp) talk to this interface.

#include <oxwald/render/features/postprocess/postprocess.hpp>
#include <oxwald/rhi/handles.hpp>

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
};
// Initialises NGX for `device` on first call (once per device; NVSDK_NGX_VULKAN_Init_with_ProjectID) and queries the
// SuperSampling capability parameters. Cached afterwards. Thread: render thread.
Status probe(rhi::Device& device);
// Process-wide: NGX was probed and failed (renderer falls back to TAAU without re-probing every frame).
[[nodiscard]] bool knownUnavailable();

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
// Records the DLSS evaluation into `cmd` (creates/recreates the feature in the same command buffer when needed).
// Returns false (and logs once) when NGX fails; the caller then falls back for this frame.
bool evaluate(rhi::Device& device, rhi::CommandList& cmd, ViewFeature& feature, const EvaluateParams& params);
// Releases a view's feature (view destroyed / DLSS switched off). Waits nothing: call after the GPU is idle or
// from deferred destruction.
void release(ViewFeature& feature);
// Shuts NGX down for this device (Renderer destruction).
void shutdown(rhi::Device& device);

} // namespace ox::render::dlss
