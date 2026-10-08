#pragma once

#include <oxwald/core/cvar.hpp>

#include <QString>

namespace ox::editor {

// Names of the rendering cvars the settings UI binds to (conventions from docs/dev/ARCHITECTURE.md §5).
namespace cvars {
inline constexpr const char* kRayTracing = "r.RayTracing";
inline constexpr const char* kRtShadows = "r.RayTracing.Shadows";
inline constexpr const char* kRtReflections = "r.RayTracing.Reflections";
inline constexpr const char* kRtAO = "r.RayTracing.AmbientOcclusion";
inline constexpr const char* kRtGI = "r.RayTracing.GlobalIllumination";
inline constexpr const char* kRtTranslucency = "r.RayTracing.Translucency";
inline constexpr const char* kAntiAliasing = "r.AntiAliasing";        // None, FXAA, TAA
inline constexpr const char* kUpscaler = "r.Upscaler";                // Off, FSR1, DLSS
inline constexpr const char* kUpscalerQuality = "r.Upscaler.Quality"; // UltraPerformance .. DLAA
inline constexpr const char* kUpscalerSharpness = "r.Upscaler.Sharpness";
inline constexpr const char* kTonemapper = "r.Tonemapper"; // ACES, AgX
inline constexpr const char* kExposure = "r.Exposure.Default";
inline constexpr const char* kAutoExposure = "r.Exposure.Auto";
inline constexpr const char* kShadowMethod = "r.Shadows.Method";
inline constexpr const char* kGIMethod = "r.GI.Method";
inline constexpr const char* kReflectionMethod = "r.Reflections.Method";
inline constexpr const char* kVSync = "r.VSync";
inline constexpr const char* kMaxFps = "r.MaxFPS";
} // namespace cvars

// Registers editor-side stand-ins for rendering cvars the render module has not registered (yet). Cvars that
// already exist (declared statically by the renderer) are left untouched, so the renderer stays the owner as
// soon as it is linked. Idempotent. Returns the number of cvars created.
int ensureRenderingCVars();

// Human readable label for an enum cvar value ("FSR1" -> "FSR 1.0", "UltraPerformance" -> "Ultra Performance").
QString cvarValueLabel(const QString& cvarName, const QString& value);

} // namespace ox::editor
