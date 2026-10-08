#pragma once

#include <oxwald/core/cvar.hpp>

#include <QString>

namespace ox::editor {

// Names of the rendering cvars the settings UI binds to (conventions from docs/dev/ARCHITECTURE.md §5).
namespace cvars {
inline constexpr const char* kRayTracing = "r.RayTracing";
inline constexpr const char* kRtShadows = "r.RayTracing.Shadows";
inline constexpr const char* kRtReflections = "r.RayTracing.Reflections";
inline constexpr const char* kRtAO = "r.RayTracing.AO";
inline constexpr const char* kRtGI = "r.RayTracing.GI";
inline constexpr const char* kRtTranslucency = "r.RayTracing.Translucency";
inline constexpr const char* kAntiAliasing = "r.AntiAliasing";        // None, FXAA, TAA
inline constexpr const char* kUpscaler = "r.Upscaler";                // Off, FSR1, DLSS, TAAU
inline constexpr const char* kUpscalerQuality = "r.Upscaler.Quality"; // UltraPerformance .. Native ("DLAA" alias)
inline constexpr const char* kUpscalerSharpness = "r.Upscaler.Sharpness";
inline constexpr const char* kTonemapper = "r.Tonemapper"; // ACES, AgX
inline constexpr const char* kExposure = "r.Exposure.Compensation";
inline constexpr const char* kAutoExposure = "r.Exposure.Auto";
inline constexpr const char* kShadows = "r.Shadows";                 // raster shadow maps
inline constexpr const char* kAOMethod = "r.AO.Method";              // 0 off, 1 SSAO, 2 GTAO
inline constexpr const char* kSSR = "r.SSR";                         // screen-space reflections
inline constexpr const char* kIrradianceVolumes = "r.GI.IrradianceVolumes";
inline constexpr const char* kVSync = "r.VSync";
inline constexpr const char* kMaxFps = "t.MaxFPS"; // runtime cvar
} // namespace cvars

// Registers editor-side stand-ins (same names, types and values as the engine's) for the cvars the Rendering page
// binds, when the render/runtime modules are not linked into this editor build. Cvars that already exist are left
// untouched, so the engine stays the owner. Idempotent. Returns the number of cvars created.
int ensureRenderingCVars();

// Human readable label for an enum cvar value ("FSR1" -> "FSR 1.0", "UltraPerformance" -> "Ultra Performance").
QString cvarValueLabel(const QString& cvarName, const QString& value);

} // namespace ox::editor
