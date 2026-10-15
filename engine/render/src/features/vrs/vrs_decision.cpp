// Variable rate shading: CPU side of the rate decision (mirrors engine/shaders/render/vrs/vrs_common.glsl), device
// availability and the pass lists.
#include <oxwald/render/features/vrs/vrs.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace ox::render::vrs {

namespace {

constexpr f32 kLumaBias = 0.1f;
constexpr f32 kQuarterFactor = 2.13f;

u32 contentAxis(f32 err, f32 meanLuma, f32 tau) {
    const f32 jnd = tau * (meanLuma + kLumaBias);
    if (kQuarterFactor * err < jnd) return 2;
    if (err < jnd) return 1;
    return 0;
}

u32 motionAxis(f32 pixels, f32 motionPixels) {
    if (motionPixels <= 0.0f) return 0;
    if (pixels >= 3.0f * motionPixels) return 2;
    if (pixels >= motionPixels) return 1;
    return 0;
}

u32 cocLevel(f32 coc) { return coc >= 6.0f ? 2u : coc >= 3.0f ? 1u : 0u; }

u32 peripheryLevel(f32 dist, f32 start) {
    if (start <= 0.0f || start >= 1.0f) return 0;
    if (dist > start + (1.0f - start) * 0.5f) return 2;
    if (dist > start) return 1;
    return 0;
}

} // namespace

u32 fitSupportedRate(u32 lw, u32 lh, u16 mask) {
    for (int i = 0; i < 5; ++i) {
        if ((mask >> encodeRate(lw, lh)) & 1u) return encodeRate(lw, lh);
        if (lw >= lh && lw > 0) --lw;
        else if (lh > 0) --lh;
    }
    return 0;
}

u32 decideRate(const TileInput& t, const DecisionParams& p) {
    u32 lw = 0, lh = 0;
    const bool adaptive = p.mode == Mode::Adaptive || p.mode == Mode::DebugOverlay;
    if (adaptive && t.statsValid) {
        const f32 ex = t.errorX / (1.0f + p.motionScale * t.motionPixelsX);
        const f32 ey = t.errorY / (1.0f + p.motionScale * t.motionPixelsY);
        const f32 strict = p.threshold * (1.0f - p.hysteresis), loose = p.threshold * (1.0f + p.hysteresis);
        const u32 sx = contentAxis(ex, t.meanLuma, strict), lx = contentAxis(ex, t.meanLuma, loose);
        const u32 sy = contentAxis(ey, t.meanLuma, strict), ly = contentAxis(ey, t.meanLuma, loose);
        if (t.prevValid) {
            lw = std::clamp(rateLog2Width(t.prevRate), sx, lx);
            lh = std::clamp(rateLog2Height(t.prevRate), sy, ly);
        } else {
            lw = sx;
            lh = sy;
        }
    }
    if (p.mode == Mode::Motion) {
        lw = std::max(lw, motionAxis(t.motionPixelsX, p.motionPixels));
        lh = std::max(lh, motionAxis(t.motionPixelsY, p.motionPixels));
    }
    if (p.mode != Mode::Foveated && p.depthOfField && t.minCocPixels > 0.0f) {
        const u32 c = cocLevel(t.minCocPixels);
        lw = std::max(lw, c);
        lh = std::max(lh, c);
    }
    const f32 start = p.mode == Mode::Foveated && p.periphery <= 0.0f ? 0.5f : p.periphery;
    const u32 f = peripheryLevel(t.peripheryDistance, start);
    lw = std::min(std::max(lw, f), p.maxLog2);
    lh = std::min(std::max(lh, f), p.maxLog2);
    return fitSupportedRate(lw, lh, p.supportedMask);
}

u32 chooseTexelSize(const rhi::DeviceCaps& caps) {
    const u32 lo = std::max(caps.shadingRateTexelMinWidth, caps.shadingRateTexelMinHeight);
    const u32 hi = std::min(caps.shadingRateTexelMaxWidth, caps.shadingRateTexelMaxHeight);
    if (lo == 0 || hi == 0 || lo > hi) return 0;
    u32 t = 8;
    while (t < lo) t *= 2;
    return t <= hi ? t : 0;
}

Availability availability(const rhi::DeviceCaps& caps) {
    Availability a;
    if (!caps.fragmentShadingRate) {
        a.reason = std::format("{} does not expose VK_KHR_fragment_shading_rate{}", caps.gpuName.empty() ? "The GPU" : caps.gpuName,
                               caps.portabilitySubset ? " (MoltenVK)" : "");
        return a;
    }
    if (!caps.attachmentShadingRate) {
        a.reason = "the device supports only pipeline / primitive shading rates, not a shading rate image";
        return a;
    }
    a.texelSize = chooseTexelSize(caps);
    if (a.texelSize == 0) {
        a.reason = std::format("no square shading rate texel size between {}x{} and {}x{}", caps.shadingRateTexelMinWidth,
                               caps.shadingRateTexelMinHeight, caps.shadingRateTexelMaxWidth, caps.shadingRateTexelMaxHeight);
        return a;
    }
    if (!caps.shadingRateSupported(2, 2)) {
        a.reason = "the device does not support 2x2 fragments";
        a.texelSize = 0;
        return a;
    }
    a.available = true;
    return a;
}

const std::vector<std::string>& defaultPasses() {
    static const std::vector<std::string> passes = {
        "ForwardOpaque",          "World.Forward",      "Sky",
        "WorldSky",               "Water.Caustics",     "Volumetrics.Composite",
        "Translucency.Refractive", "Translucency.OIT",  "Translucency.Sorted",
        "Water.Surface",          "Particles.Render",
    };
    return passes;
}

std::vector<std::string> parsePassList(std::string_view value) {
    if (value.find_first_not_of(" 	,") == std::string_view::npos) return defaultPasses();
    std::vector<std::string> out;
    while (!value.empty()) {
        const usize comma = value.find(',');
        std::string_view item = value.substr(0, comma);
        value = comma == std::string_view::npos ? std::string_view{} : value.substr(comma + 1);
        const usize b = item.find_first_not_of(" 	"), e = item.find_last_not_of(" 	");
        if (b != std::string_view::npos) out.emplace_back(item.substr(b, e - b + 1));
    }
    return out;
}

bool isExcludedPass(std::string_view name) {
    // Depth / G-buffer outputs feed TAA, DLSS, SSR, SSAO and picking with per-pixel data; shadow maps are depth only;
    // post-processing, tonemapping, upscalers, UI and editor overlays are resolution-exact.
    static constexpr std::string_view kPrefixes[] = {
        "DepthPrepass", "World.Prepass", "Shadow", "World.Shadow", "HiZ",       "Translucency.BackfaceDepth",
        "Translucency.DepthCopy", "Particles.LowResDepth", "Particles.Composite", "TAA", "TAAU", "DLSS", "FSR",
        "Resample", "Tonemap", "Final", "Pick", "EditorGrid", "Selection", "DebugLines", "DebugView", "Overdraw",
        "Wireframe", "UI", "Ui", "VRS.", "PostComposite", "LdrPost", "Bloom", "DOF", "MotionBlur", "Underwater",
        "Water.Underwater", "Vegetation.ImpostorBake", "Reflections.", "IBL", "RT.",
    };
    for (std::string_view p : kPrefixes) {
        if (name.substr(0, p.size()) == p) return true;
    }
    return false;
}

} // namespace ox::render::vrs
