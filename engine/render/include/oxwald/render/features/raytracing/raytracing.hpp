#pragma once

// Ray tracing ("RTX mode") feature area: acceleration structures, ray traced shadows / reflections / AO / GI (DDGI) /
// translucency / volumetric visibility, a progressive path tracer and ReSTIR DI, plus the SVGF-style denoiser that the
// effects share (the denoiser also runs on devices without ray tracing).
//
// Gating: every ray traced feature is enabled only when `RenderSettings::rayTracing` (cvar r.RayTracing, forced off by
// the renderer on devices without ray queries) AND `DeviceCaps::rayTracingSupported()` AND its own r.RayTracing.<Effect>
// cvar are set. Each effect joins the exclusive group of its raster counterpart with a higher priority, so turning
// r.RayTracing on/off at runtime simply swaps variants on the next frame (the render graph is re-declared every frame).
//
// See docs/dev/modules/render.md, section "Ray tracing".

#include <oxwald/core/types.hpp>
#include <oxwald/rhi/device_caps.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace ox::render {

class FeatureRegistry;
struct RenderSettings;

namespace rt {

// Exclusive groups shared with the raster variants (keep in sync with the owning teams' features).
inline constexpr std::string_view kGroupShadows = "Shadows";             // ShadowsRaster (core)
inline constexpr std::string_view kGroupAO = "AO";                       // SSAO / GTAO (reflections-ao)
inline constexpr std::string_view kGroupReflections = "Reflections";     // SSR + probes (reflections-ao)
inline constexpr std::string_view kGroupIndirectDiffuse = "IndirectDiffuse"; // probe GI (reflections-ao)
inline constexpr std::string_view kGroupTranslucency = "Translucency";   // OIT / screen-space refraction
inline constexpr i32 kRtPriority = 100;                             // raster variants use 0

// Additional graph resources published by this area.
namespace res {
// Buffer: the per-frame ray tracing scene header (TLAS address, instance table, DDGI volume). Ray traced passes
// declare `.read(buffer, Access::AccelStructRead)` on it so they are ordered after the TLAS build.
inline constexpr std::string_view kRtScene = "RtScene";
// The froxel visibility for the volumetrics team is published under their documented name
// volumetrics::kVolumetricFogVisibility ("VolumetricFogVisibility": 3D RGBA16F at their froxel grid, r = sun,
// g = local light shadowed/unshadowed ratio, b = sky visibility) by "VolumetricsRT".
// Persistent RGBA8 screen-space visibility of shadowed local lights (GpuShadow kind 2, channel in atlasRect.x).
inline constexpr std::string_view kLocalLightShadows = "RtLocalLightShadows";
} // namespace res

// Snapshot of every r.RayTracing.* / r.PathTracing.* cvar (taken once per frame, like RenderSettings).
struct RtSettings {
    bool shadows = true;
    i32 shadowSpp = 1;
    i32 shadowMaxLocalLights = 4; // explicit per-light visibility channels (0..4)
    bool shadowReSTIR = false;    // ReSTIR DI ratio estimator for every shadowed local light
    bool shadowColored = true;    // coloured transmission through translucent casters
    bool shadowDenoiser = true;
    i32 shadowResolutionScale = 100;

    bool reflections = true;
    f32 reflectionMaxRoughness = 0.6f;
    i32 reflectionSpp = 1;
    i32 reflectionResolutionScale = 100;
    bool reflectionDenoiser = true;

    bool ao = true;
    f32 aoRadius = 1.0f;
    i32 aoSpp = 1;
    i32 aoResolutionScale = 100;
    bool aoDenoiser = true;

    bool gi = true;
    i32 giRaysPerProbe = 128;
    i32 giProbesXZ = 24;
    i32 giProbesY = 8;
    f32 giProbeSpacing = 2.0f;
    f32 giHysteresis = 0.97f;
    i32 giResolutionScale = 100;

    bool translucency = true;
    i32 translucencyMaxBounces = 4;

    bool volumetrics = true;
    i32 volumetricLocalSamples = 2; // light samples per froxel for the local light visibility ratio
    i32 volumetricSkyRays = 1;

    i32 denoiserIterations = 4; // À-trous passes (1..5)
    i32 denoiserMaxHistory = 32;
    i32 blasLod = -1;           // -1 = coarsest LOD, otherwise the requested LOD clamped to the mesh
    i32 blasBuildsPerFrame = 16;

    bool pathTracing = false;
    i32 pathTracingMode = 0; // 0 ray query megakernel, 1 RT pipeline + SBT
    i32 pathMaxBounces = 8;
    i32 pathSamplesPerFrame = 1;
    i32 pathMaxSamples = 4096;

    [[nodiscard]] static RtSettings fromCVars();
};

// True when ray traced features may run this frame (r.RayTracing after the renderer forced it off without support,
// and the device capabilities).
[[nodiscard]] bool rayTracingActive(const RenderSettings& settings, const rhi::DeviceCaps& caps);

// True when "TranslucencyRT" replaces the raster refraction of refractive materials this frame (the raster
// Translucency feature may skip drawing refractive materials then; transparent ones stay raster).
[[nodiscard]] bool rayTracedRefractionActive(const RenderSettings& settings, const rhi::DeviceCaps& caps);

// Availability for UI: whether each effect could run on this device and why not.
struct RtEffectStatus {
    std::string name;    // "Shadows", "Reflections", ...
    std::string cvar;    // r.RayTracing.Shadows
    bool available = false;
    std::string reason;  // empty when available
};
struct RtStatus {
    bool available = false;
    std::string reason; // DeviceCaps::whyRayTracingUnavailable() (human readable), empty when available
    std::vector<RtEffectStatus> effects;
};
[[nodiscard]] RtStatus rayTracingStatus(const rhi::DeviceCaps& caps);

// Names of every cvar of this area (settings UI grouping).
[[nodiscard]] std::vector<std::string> rayTracingCVarNames();

// Forces registration of the area's cvars (file statics). Called by registerRayTracingFeatures().
void registerRayTracingCVars();

} // namespace rt

// Adds every ray tracing feature to a renderer's registry (called from registerBuiltinFeatures()).
void registerRayTracingFeatures(FeatureRegistry& registry);

} // namespace ox::render
