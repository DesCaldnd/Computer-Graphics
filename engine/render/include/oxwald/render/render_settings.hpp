#pragma once

// RenderSettings: a plain snapshot of every renderer cvar, taken once per frame (RenderSettings::fromCVars()).
// Passes and features read the snapshot, never the cvars directly, so a frame is internally consistent even when
// the console changes values mid-frame. Feature-owned cvars live next to the feature; read them in
// IRenderFeature::isEnabled()/setup() (cvars are atomics).

#include <oxwald/core/types.hpp>

#include <string>

namespace ox::render {

enum class Tonemapper : i32 { ACES = 0, AgX = 1, Neutral = 2, Linear = 3 };
enum class ExposureMode : i32 { Camera = 0, Manual = 1 };

enum class DebugView : i32 {
    None = 0,
    Albedo,
    Normals,
    Roughness,
    Metallic,
    AO,
    Emissive,
    LightComplexity, // clustered light count heat map
    Overdraw,
    ShadowCascades,
    Wireframe, // requires DeviceCaps::fillModeNonSolid
    Velocity,
    Depth,
    ShadowMask,
    Count
};
const char* debugViewName(DebugView v);

struct RenderSettings {
    // --- resolution ---
    f32 screenPercentage = 100.0f; // r.ScreenPercentage: render resolution = output * pct / 100

    // --- shading ---
    Tonemapper tonemapper = Tonemapper::ACES;
    ExposureMode exposureMode = ExposureMode::Camera;
    f32 manualEV100 = 10.0f;
    f32 exposureCompensation = 0.0f;
    bool ibl = true;
    f32 iblIntensity = 1.0f;
    i32 iblResolution = 128;      // prefiltered cube face size
    bool multiScatter = true;     // GGX energy compensation
    bool depthPrepass = true;     // always on for forward+ (kept as a cvar for experiments)
    i32 maxLights = 4096;         // local lights uploaded per frame
    i32 clusterMaxLights = 256;   // per cluster (shader constant in view constants)
    f32 clusterMaxDistance = 500.0f; // far bound of the cluster grid for infinite projections

    // --- shadows ---
    bool shadows = true;
    i32 csmResolution = 2048;
    i32 csmCascades = 4;
    f32 csmDistance = 120.0f;
    f32 csmLambda = 0.75f;   // practical split scheme: 0 = uniform, 1 = logarithmic
    f32 csmBlend = 0.1f;     // fraction of a cascade used for blending into the next
    i32 atlasSize = 4096;    // spot light atlas
    i32 pointResolution = 512; // cube face size of point light shadows
    i32 maxShadowedLights = 16; // spot + point
    i32 maxPointShadows = 8;
    i32 pcfTaps = 16;        // Poisson taps (0 = single hardware compare)
    bool pcss = true;
    f32 shadowFilterRadius = 1.5f; // PCF radius in texels
    i32 spotMinResolution = 128;
    i32 spotMaxResolution = 1024;
    bool shadowCaching = true;

    // --- textures ---
    i32 anisotropy = 8;
    f32 mipBias = 0.0f;
    i32 maxTextureSize = 8192;

    // --- view distance ---
    f32 drawDistance = 0.0f; // 0 = camera far plane
    f32 lodBias = 0.0f;

    // --- anti-aliasing / upscaling hooks (implemented by features) ---
    i32 antiAliasing = 0; // r.AntiAliasing: 0 None, 1 FXAA, 2 TAA (features read it)
    i32 upscaler = 0;     // r.Upscaler: Off, FSR1, DLSS, TAAU
    i32 upscalerQuality = 3;
    bool rayTracing = false; // r.RayTracing (forced false when DeviceCaps lack support)

    // --- debug / editor ---
    DebugView debugView = DebugView::None;
    bool wireframe = false;
    bool gpuTimings = true;
    bool frustumCulling = true;
    bool sky = true;

    // Hash of the values that change the frame graph topology or persistent allocations (diagnostics/tests).
    [[nodiscard]] u64 topologyHash() const;

    // Snapshot of the current cvar values.
    [[nodiscard]] static RenderSettings fromCVars();
};

// Forces registration of every render cvar (they are file-statics in settings.cpp). Called by Renderer::create;
// call it yourself when only settings are needed (e.g. a launcher's quality dialog).
void registerRenderCVars();

} // namespace ox::render
