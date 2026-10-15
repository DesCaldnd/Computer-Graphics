#pragma once

// Post-processing, anti-aliasing and upscaling (area postprocess-upscalers).
//
// Frame chain (per view; see docs/dev/modules/render.md "Post-processing & upscaling"):
//   PreDepth          AutoExposure (pre-exposure for this frame from the GPU result of an earlier frame)
//   BeforePostProcess TAA (render res) | TAAU / DLSS temporal upscalers (render → output res, before post)
//   PostProcess       AutoExposure histogram (100) → DepthOfField (200) → MotionBlur (300) → Bloom (400)
//   Upscale           FSR1 (EASU + RCAS) | temporal upscalers reserve the slot (no bilinear resample)
//   AfterUpscale      CAS sharpen (50) → HDR composite (100): chromatic aberration, bloom + lens dirt, vignette,
//                     white balance + grading LUT (32³, log space)
//   [core Tonemap]
//   Overlay (first)   LDR post: FXAA, user LUT texture (display referred), film grain
//
// Upscalers: r.Upscaler (Off, FSR1, DLSS, TAAU) + r.Upscaler.Quality (UltraPerformance 33 %, Performance 50 %,
// Balanced 58 %, Quality 67 %, Native/DLAA 100 %) drive the render resolution, the jitter sequence and the material
// mip bias. DLSS needs an NVIDIA RTX GPU on Windows/Linux (NGX); elsewhere it reports why it is unavailable and
// r.Upscaler=DLSS falls back to TAAU.

#include <oxwald/core/cvar.hpp>
#include <oxwald/render/components/postprocess.hpp>
#include <oxwald/render/render_types.hpp>
#include <oxwald/rhi/device_caps.hpp>

#include <array>
#include <span>
#include <string>
#include <vector>

namespace ox {
struct CameraComponent;
}
namespace ox::rhi {
class Device;
struct DeviceDesc;
} // namespace ox::rhi

namespace ox::render {

class FeatureRegistry;
class RenderView;
struct RenderSnapshot;
struct RenderSettings;
struct CameraParams;

// Adds every post-process / AA / upscaler feature to a renderer (called from registerBuiltinFeatures).
void registerPostProcessFeatures(FeatureRegistry& registry);

// --- upscalers ------------------------------------------------------------------------------------------------

enum class UpscalerType : i32 { Off = 0, FSR1 = 1, DLSS = 2, TAAU = 3 };
enum class UpscalerQuality : i32 { UltraPerformance = 0, Performance = 1, Balanced = 2, Quality = 3, Native = 4 };

[[nodiscard]] const char* upscalerName(UpscalerType type);
// Render scale per quality mode: 0.333, 0.5, 0.58, 0.667, 1.0.
[[nodiscard]] f32 upscalerRenderScale(UpscalerQuality quality);
// Material texture mip bias for a render/output ratio: log2(render/output), minus one more for DLSS (NVIDIA's
// guidance), FSR 1 and TAAU use log2(render/output) (AMD's FSR 1 table: -0.38 … -1.0).
[[nodiscard]] f32 upscalerMipBias(UpscalerType type, f32 renderScale);
// Halton jitter period for an upscale ratio (NVIDIA: 8 × (output/render)², clamped to [8, 64]).
[[nodiscard]] u32 upscalerJitterPhases(f32 renderScale);

struct UpscalerAvailability {
    UpscalerType type = UpscalerType::Off;
    std::string name;      // "AMD FSR 1.0", "NVIDIA DLSS", ...
    bool available = false;
    bool temporal = false; // consumes jitter + motion vectors (replaces TAA)
    std::string reason;    // why it is unavailable (empty when available)
};
// Per-upscaler availability for the settings UI. With a device, DLSS is probed through NGX (initialised once per
// device); without one only static reasons (platform, build, GPU vendor) are known.
[[nodiscard]] std::vector<UpscalerAvailability> upscalerAvailability(rhi::Device* device = nullptr);
[[nodiscard]] UpscalerAvailability upscalerAvailability(UpscalerType type, rhi::Device* device = nullptr);

// Vulkan instance/device extensions NGX needs (empty where DLSS cannot run). Call before rhi::Device::create.
void appendUpscalerVulkanExtensions(rhi::DeviceDesc& desc);

// --- DLSS Ray Reconstruction ------------------------------------------------------------------------------------
//
// r.Denoiser (SVGF, DLSSRR, Auto; Auto = Ray Reconstruction where it can run). With r.RayTracing on (and the path
// tracer off) and RR selected and available, the "DLSSRR" feature takes the upscaler slot: the ray traced shadows,
// AO and reflections skip their SVGF passes, the noisy lit frame plus guide buffers (diffuse / specular albedo,
// normals + roughness, hardware depth, motion vectors, reflection hit distance, exposure) go to NGX
// (NVSDK_NGX_Feature_RayReconstruction), which denoises and upscales in one pass. Render resolution follows
// r.Upscaler.Quality when r.Upscaler is set, DLAA (native) when r.Upscaler is Off. Everywhere else (non-NVIDIA,
// MoltenVK, missing nvngx_dlssd runtime, old driver) the SVGF path stays.

// Neural: the SVGF pipeline with a learned À-trous kernel (render/neural, vendor neutral).
enum class DenoiserType : i32 { SVGF = 0, DLSSRR = 1, Auto = 2, Neural = 3 };

struct RayReconstructionAvailability {
    bool available = false;
    std::string reason; // why it cannot run (empty when available)
};
// With a device, NGX is probed (once per device); without one only static reasons (build, platform) are known.
[[nodiscard]] RayReconstructionAvailability rayReconstructionAvailability(rhi::Device* device = nullptr);
// NVIDIA NGX feature instances (DLSS super resolution + Ray Reconstruction, all views) alive right now; 0 without NGX.
// Diagnostics and leak tests: switching a feature off must give its instance back.
[[nodiscard]] u32 ngxLiveFeatureCount();
// r.Denoiser asks for Ray Reconstruction and it can run with these settings on this device (ray tracing active,
// path tracer off, NVIDIA, not known to be unavailable). Drives feature gating.
[[nodiscard]] bool rayReconstructionSelected(const RenderSettings& settings, const rhi::DeviceCaps& caps);
// Ray Reconstruction denoises this view's current frame: set by the DLSSRR feature while it prepares the view, so
// it is true only when the feature really runs. The ray traced effects use it to skip their SVGF denoisers.
[[nodiscard]] bool rayReconstructionRunsThisFrame(const RenderView& view);

// Guide resources for Ray Reconstruction (render resolution), published by the ray tracing area.
namespace res {
inline constexpr std::string_view kRrDiffuseAlbedo = "RR.DiffuseAlbedo";   // RGBA8: baseColor × (1 − metallic)
inline constexpr std::string_view kRrSpecularAlbedo = "RR.SpecularAlbedo"; // RGBA8: pre-integrated GGX reflectance
inline constexpr std::string_view kRrSpecularHitDistance = "RR.SpecularHitDistance"; // RGBA16F, r = metres
} // namespace res

// --- quality presets ("Auto") ---------------------------------------------------------------------------------

struct RecommendedSettings {
    std::array<QualityLevel, kScalabilityGroupCount> levels{};
    QualityLevel overall = QualityLevel::High;
    i32 antiAliasing = 2; // r.AntiAliasing
    UpscalerType upscaler = UpscalerType::Off;
    UpscalerQuality upscalerQuality = UpscalerQuality::Quality;
    f32 screenPercentage = 100.0f; // effective render scale (informational; upscalers set it from the quality)
    DenoiserType denoiser = DenoiserType::Auto; // r.Denoiser: Ray Reconstruction where it can run, else SVGF
    std::string rationale;         // human readable explanation for the UI / log
};
// Combines the GPU benchmark score (render::runGpuBenchmark, 100 = GTX 1060 class) with upscaler availability:
// RTX GPUs get DLSS (Ultra: DLAA, High: Quality, Medium: Balanced, Low: Performance); elsewhere High/Ultra render
// natively with TAA, Medium uses TAAU Quality and Low FSR 1 Balanced (+ TAA at render resolution).
[[nodiscard]] RecommendedSettings recommendedSettings(const rhi::DeviceCaps& caps, f64 benchmarkScore,
                                                      bool dlssAvailable);
// Convenience: availability probed through the device (NGX) when a device is given.
[[nodiscard]] RecommendedSettings recommendedSettings(rhi::Device& device, f64 benchmarkScore);
// Applies the levels (scalability::setGroup) and the AA/upscaler cvars.
void applyRecommendedSettings(const RecommendedSettings& settings);

// --- post-process volumes -------------------------------------------------------------------------------------

struct PostProcessVolumeSnapshot {
    PostProcessSettings settings;
    glm::mat4 worldToLocal{1.0f}; // rotation + translation only (scale folded into `extents`)
    glm::vec3 extents{5.0f};
    f32 blendRadius = 1.0f;
    f32 blendWeight = 1.0f;
    i32 priority = 0;
    bool unbound = true;
};

// Weight of a volume at a world position (0..blendWeight).
[[nodiscard]] f32 postProcessVolumeWeight(const PostProcessVolumeSnapshot& volume, const glm::vec3& position);
// Base settings from the cvars (r.Exposure.*, r.Bloom.Intensity, r.Sharpen, ...).
[[nodiscard]] PostProcessSettings postProcessDefaults(const RenderSettings& settings);
// Blends `volumes` over `base` for a camera at `position` (ascending priority, per-category overrides).
[[nodiscard]] PostProcessSettings blendPostProcessVolumes(const PostProcessSettings& base,
                                                          std::span<const PostProcessVolumeSnapshot> volumes,
                                                          const glm::vec3& position);
// Snapshot volumes (if any) blended over the cvar defaults for this camera. With `camera`, its depth of field
// (CameraComponent::focusDistance/focalLength) is the base that DoF volumes override.
[[nodiscard]] PostProcessSettings resolvePostProcessSettings(const RenderSnapshot& snapshot,
                                                             const RenderSettings& settings, const glm::vec3& position,
                                                             const CameraComponent* camera = nullptr);

} // namespace ox::render
