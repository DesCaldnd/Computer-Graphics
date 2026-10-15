#pragma once

// Quality profile for mobile GPUs (Android): replaces the desktop synthetic benchmark on first launch. Pure
// function of the DeviceCaps (host-tested):
//
//   * tier from the GPU: Adreno 7xx/8xx (Snapdragon 8 Gen 2+), Mali-G7xx / Immortalis, Xclipse, PowerVR DXT ->
//     Medium; everything else (older Adreno/Mali, emulators, SwiftShader) -> Low. Nothing goes above Medium:
//     phones are thermally limited, sustained performance matters more than peak.
//   * ray tracing (sg.RayTracing above Low) only when the device has VK_KHR_ray_query + acceleration structures
//     (Adreno 740+/750, Immortalis-G715+), and even then left at Low unless `allowRayTracing`.
//   * mesh shaders: the gpu-driven path already falls back to the vertex pipeline when DeviceCaps::meshShader is
//     false (most Android drivers); r.MeshShaders is switched off explicitly for the log / settings UI.
//   * upscaling: FSR 1 (spatial, cheap) at Quality (Medium) / Balanced (Low), TAA at render resolution.
#include <oxwald/render/features/postprocess/postprocess.hpp>
#include <oxwald/rhi/device_caps.hpp>

#include <string>
#include <utility>
#include <vector>

namespace ox::render {

enum class MobileGpuTier : u8 { Low, Medium };

struct MobileQualityProfile {
    MobileGpuTier tier = MobileGpuTier::Low;
    RecommendedSettings settings;                           // levels, AA, upscaler (applyRecommendedSettings)
    std::vector<std::pair<std::string, std::string>> cvars; // extra overrides (r.MeshShaders, r.RayTracing, ...)
    bool rayTracing = false;
    bool emulator = false;
    std::string rationale;
};

struct MobileQualityOptions {
    bool allowRayTracing = false; // let capable devices keep ray traced effects (thermals!)
};

// Adreno model number from a driver GPU name ("Adreno (TM) 750" -> 750), 0 when not an Adreno.
[[nodiscard]] u32 adrenoModel(std::string_view gpuName);
[[nodiscard]] bool isEmulatorGpu(const rhi::DeviceCaps& caps);
[[nodiscard]] MobileGpuTier mobileGpuTier(const rhi::DeviceCaps& caps);
[[nodiscard]] MobileQualityProfile mobileQualityProfile(const rhi::DeviceCaps& caps, const MobileQualityOptions& options = {});
// applyRecommendedSettings + the extra cvars (CVarSource::Code).
void applyMobileQualityProfile(const MobileQualityProfile& profile);

} // namespace ox::render
