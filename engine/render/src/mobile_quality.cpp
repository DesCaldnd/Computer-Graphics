#include <oxwald/core/cvar.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/core/scalability.hpp>
#include <oxwald/render/mobile_quality.hpp>
#include <oxwald/render/quality.hpp>

#include <algorithm>
#include <cctype>
#include <format>

namespace ox::render {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// First number after `key` in `s` (both lower case), 0 when absent.
u32 numberAfter(std::string_view s, std::string_view key) {
    const usize at = s.find(key);
    if (at == std::string_view::npos) return 0;
    usize i = at + key.size();
    // Allow a short separator such as " (tm) " ("Adreno (TM) 750").
    for (usize skipped = 0; i < s.size() && !std::isdigit(static_cast<unsigned char>(s[i])); ++i, ++skipped) {
        if (skipped >= 8) return 0;
    }
    u32 n = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) n = n * 10 + u32(s[i++] - '0');
    return n;
}

} // namespace

u32 adrenoModel(std::string_view gpuName) { return numberAfter(lower(gpuName), "adreno"); }

bool isEmulatorGpu(const rhi::DeviceCaps& caps) {
    const std::string name = lower(caps.gpuName);
    const std::string driver = lower(caps.driverName + " " + caps.driverInfo);
    return name.find("swiftshader") != std::string::npos || name.find("emulator") != std::string::npos ||
           name.find("gfxstream") != std::string::npos || driver.find("gfxstream") != std::string::npos ||
           name.find("llvmpipe") != std::string::npos || name.find("lavapipe") != std::string::npos;
}

MobileGpuTier mobileGpuTier(const rhi::DeviceCaps& caps) {
    if (isEmulatorGpu(caps)) return MobileGpuTier::Low;
    const std::string name = lower(caps.gpuName);
    if (const u32 adreno = adrenoModel(caps.gpuName); adreno > 0) {
        // Adreno 730 (8 Gen 1) and newer, incl. 740 / 750 (8 Gen 3, OnePlus 12) / 830.
        return adreno >= 730 ? MobileGpuTier::Medium : MobileGpuTier::Low;
    }
    if (name.find("immortalis") != std::string::npos || name.find("xclipse") != std::string::npos) return MobileGpuTier::Medium;
    if (const u32 mali = numberAfter(name, "mali-g"); mali >= 710) return MobileGpuTier::Medium;
    if (name.find("powervr") != std::string::npos && name.find("dxt") != std::string::npos) return MobileGpuTier::Medium;
    // Unknown vendor with a big GPU (e.g. desktop GPU through an x86_64 emulator passthrough) is handled by
    // isEmulatorGpu(); anything else unknown stays Low.
    return MobileGpuTier::Low;
}

MobileQualityProfile mobileQualityProfile(const rhi::DeviceCaps& caps, const MobileQualityOptions& options) {
    MobileQualityProfile p;
    p.emulator = isEmulatorGpu(caps);
    p.tier = mobileGpuTier(caps);
    const bool medium = p.tier == MobileGpuTier::Medium;
    p.rayTracing = options.allowRayTracing && caps.rayTracingSupported();
    // Scores on the desktop benchmark scale (35 = Medium threshold): Medium tier -> Medium, Low tier -> Low.
    p.settings.levels = levelsForScore(medium ? 40.0 : 10.0, p.rayTracing);
    p.settings.levels[usize(Scalability::GlobalIllumination)] = QualityLevel::Low;
    p.settings.levels[usize(Scalability::Volumetrics)] = QualityLevel::Low;
    if (!p.rayTracing) p.settings.levels[usize(Scalability::RayTracing)] = QualityLevel::Low;
    p.settings.overall = medium ? QualityLevel::Medium : QualityLevel::Low;
    p.settings.antiAliasing = 2; // TAA at render resolution (FSR 1 consumes its output)
    p.settings.upscaler = UpscalerType::FSR1;
    p.settings.upscalerQuality = medium ? UpscalerQuality::Quality : UpscalerQuality::Balanced;
    p.settings.screenPercentage = upscalerRenderScale(p.settings.upscalerQuality) * 100.0f;

    p.cvars.emplace_back("r.RayTracing", p.rayTracing ? "1" : "0");
    if (!(caps.meshShader && caps.taskShader)) p.cvars.emplace_back("r.GpuDriven.MeshShaders", "0");
    p.rationale = std::format("mobile GPU '{}' ({}{}): {} preset, FSR 1 {} ({:.0f} %), ray tracing {}{}", caps.gpuName,
                              p.emulator ? "emulator, " : "", medium ? "high-end tier" : "low tier",
                              medium ? "Medium" : "Low", medium ? "Quality" : "Balanced", p.settings.screenPercentage,
                              p.rayTracing ? "on" : (caps.rayTracingSupported() ? "off (thermals)" : "unsupported"),
                              caps.meshShader ? "" : ", vertex pipeline (no mesh shaders)");
    p.settings.rationale = p.rationale;
    return p;
}

void applyMobileQualityProfile(const MobileQualityProfile& profile) {
    applyRecommendedSettings(profile.settings);
    CVarRegistry& reg = CVarRegistry::instance();
    for (const auto& [name, value] : profile.cvars) reg.set(name, value, CVarSource::Code);
}

} // namespace ox::render
