// Upscaler availability, quality modes and the "Auto" quality recommendation.
#include "../pp_common.hpp"
#include "dlss_backend.hpp"

#include <oxwald/core/log.hpp>
#include <oxwald/core/scalability.hpp>
#include <oxwald/render/quality.hpp>
#include <oxwald/render/render_settings.hpp>

#include <cmath>
#include <format>

namespace ox::render {

const char* upscalerName(UpscalerType type) {
    switch (type) {
    case UpscalerType::Off: return "Off";
    case UpscalerType::FSR1: return "AMD FSR 1.0";
    case UpscalerType::DLSS: return "NVIDIA DLSS";
    case UpscalerType::TAAU: return "Temporal Upsampling (TAAU)";
    }
    return "?";
}

f32 upscalerRenderScale(UpscalerQuality q) {
    switch (q) {
    case UpscalerQuality::UltraPerformance: return 1.0f / 3.0f;
    case UpscalerQuality::Performance: return 0.5f;
    case UpscalerQuality::Balanced: return 0.58f;
    case UpscalerQuality::Quality: return 2.0f / 3.0f;
    case UpscalerQuality::Native: return 1.0f;
    }
    return 1.0f;
}

f32 upscalerMipBias(UpscalerType type, f32 renderScale) {
    if (type == UpscalerType::Off || renderScale <= 0.0f) return 0.0f;
    const f32 b = std::log2(std::min(renderScale, 1.0f));
    return type == UpscalerType::DLSS ? b - 1.0f : b;
}

u32 upscalerJitterPhases(f32 renderScale) {
    const f32 ratio = 1.0f / std::clamp(renderScale, 0.1f, 1.0f);
    return u32(std::clamp(std::ceil(8.0f * ratio * ratio), 8.0f, 64.0f));
}

UpscalerAvailability upscalerAvailability(UpscalerType type, rhi::Device* device) {
    UpscalerAvailability a;
    a.type = type;
    a.name = upscalerName(type);
    switch (type) {
    case UpscalerType::Off: a.available = true; break;
    case UpscalerType::FSR1: a.available = true; break; // compute shaders only
    case UpscalerType::TAAU:
        a.available = true;
        a.temporal = true;
        break;
    case UpscalerType::DLSS: {
        a.temporal = true;
        if (!dlss::compiled()) {
            a.reason = dlss::buildUnavailableReason();
        } else if (device) {
            const dlss::Status s = dlss::probe(*device);
            a.available = s.available;
            a.reason = s.reason;
        } else {
            a.reason = "DLSS availability is known once a Vulkan device exists (requires an NVIDIA RTX GPU)";
        }
        break;
    }
    }
    return a;
}

std::vector<UpscalerAvailability> upscalerAvailability(rhi::Device* device) {
    return {upscalerAvailability(UpscalerType::Off, device), upscalerAvailability(UpscalerType::FSR1, device),
            upscalerAvailability(UpscalerType::DLSS, device), upscalerAvailability(UpscalerType::TAAU, device)};
}

void appendUpscalerVulkanExtensions(rhi::DeviceDesc& desc) {
    std::vector<std::string> inst, dev;
    dlss::requiredExtensions(inst, dev);
    for (std::string& e : inst) desc.optionalInstanceExtensions.push_back(std::move(e));
    for (std::string& e : dev) desc.optionalDeviceExtensions.push_back(std::move(e));
}

RecommendedSettings recommendedSettings(const rhi::DeviceCaps& caps, f64 score, bool dlssAvailable) {
    RecommendedSettings r;
    r.levels = levelsForScore(score, caps.rayTracingSupported());
    QualityLevel base = QualityLevel::Low;
    if (score >= 160.0) base = QualityLevel::Ultra;
    else if (score >= 80.0) base = QualityLevel::High;
    else if (score >= 35.0) base = QualityLevel::Medium;
    r.overall = base;
    r.antiAliasing = 2; // TAA (temporal upscalers replace it, FSR 1 consumes its output)
    if (dlssAvailable) {
        r.upscaler = UpscalerType::DLSS;
        r.upscalerQuality = base == QualityLevel::Ultra    ? UpscalerQuality::Native
                            : base == QualityLevel::High   ? UpscalerQuality::Quality
                            : base == QualityLevel::Medium ? UpscalerQuality::Balanced
                                                           : UpscalerQuality::Performance;
        r.rationale = std::format("RTX GPU (score {:.0f}, {}): DLSS {}", score, scalability::levelName(base),
                                  r.upscalerQuality == UpscalerQuality::Native ? "DLAA" : "super resolution");
    } else if (base == QualityLevel::Ultra || base == QualityLevel::High) {
        r.upscaler = UpscalerType::Off;
        r.upscalerQuality = UpscalerQuality::Native;
        r.rationale = std::format("score {:.0f} ({}): native resolution with TAA", score, scalability::levelName(base));
    } else if (base == QualityLevel::Medium) {
        r.upscaler = UpscalerType::TAAU;
        r.upscalerQuality = UpscalerQuality::Quality;
        r.rationale = std::format("score {:.0f} (Medium): TAAU at 67 % (DLSS unavailable)", score);
    } else {
        r.upscaler = UpscalerType::FSR1;
        r.upscalerQuality = UpscalerQuality::Balanced;
        r.rationale = std::format("score {:.0f} (Low): FSR 1 at 58 % + TAA (DLSS unavailable)", score);
    }
    r.screenPercentage = r.upscaler == UpscalerType::Off ? 100.0f : upscalerRenderScale(r.upscalerQuality) * 100.0f;
    return r;
}

RecommendedSettings recommendedSettings(rhi::Device& device, f64 score) {
    const bool dlssOk = upscalerAvailability(UpscalerType::DLSS, &device).available;
    return recommendedSettings(device.caps(), score, dlssOk);
}

void applyRecommendedSettings(const RecommendedSettings& r) {
    for (usize g = 0; g < kScalabilityGroupCount; ++g) scalability::setGroup(Scalability(g), r.levels[g]);
    CVarRegistry& reg = CVarRegistry::instance();
    reg.set("r.AntiAliasing", std::to_string(r.antiAliasing), CVarSource::Code);
    reg.set("r.Upscaler", std::to_string(i32(r.upscaler)), CVarSource::Code);
    reg.set("r.Upscaler.Quality", std::to_string(i32(r.upscalerQuality)), CVarSource::Code);
    if (r.upscaler == UpscalerType::Off) reg.set("r.ScreenPercentage", "100", CVarSource::Code);
    OX_LOG_INFO("render", "recommended settings: {}", r.rationale);
}

namespace pp {

bool dlssUsable(const rhi::DeviceCaps& caps) {
    return dlss::compiled() && caps.vendor == rhi::GpuVendor::Nvidia && !dlss::knownUnavailable();
}

UpscalerType effectiveUpscaler(const RenderSettings& s, const rhi::DeviceCaps& caps) {
    switch (UpscalerType(s.upscaler)) {
    case UpscalerType::FSR1: return UpscalerType::FSR1;
    case UpscalerType::TAAU: return UpscalerType::TAAU;
    case UpscalerType::DLSS: return dlssUsable(caps) ? UpscalerType::DLSS : UpscalerType::TAAU;
    default: return UpscalerType::Off;
    }
}

bool temporalUpscalerActive(const RenderSettings& s, const rhi::DeviceCaps& caps) {
    const UpscalerType u = effectiveUpscaler(s, caps);
    return u == UpscalerType::TAAU || u == UpscalerType::DLSS;
}

} // namespace pp

} // namespace ox::render
