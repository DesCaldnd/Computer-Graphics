#include "settings/render_cvars.hpp"

#include "core/common.hpp"

#include <QHash>

namespace ox::editor {

namespace {

template <class T, class... Args>
int make(const char* name, Args&&... args) {
    if (CVarRegistry::instance().find(name)) return 0;
    // Intentionally leaked: the registry stores raw pointers and outlives every widget.
    new CVar<T>(name, std::forward<Args>(args)...);
    return 1;
}

template <class T>
int makeLv(const char* name, T def, const char* desc, Scalability group, std::initializer_list<T> levels) {
    if (CVarRegistry::instance().find(name)) return 0;
    new CVar<T>(name, def, desc, group, levels, CVarFlags::Persist);
    return 1;
}

constexpr CVarFlags P = CVarFlags::Persist;
using S = Scalability;

} // namespace

int ensureRenderingCVars() {
    // Mirrors of the engine declarations (engine/render/src/settings.cpp, features/raytracing/rt_settings.cpp,
    // features/reflections, runtime settings.cpp) for editor builds without those modules. Nothing else is created:
    // stand-ins for cvars no engine code reads would only clutter the console and the scalability page.
    int n = 0;
    using RT = Scalability;
    n += make<bool>(cvars::kRayTracing, false, "Ray traced effects (ignored when the GPU lacks ray queries)", P);
    n += makeLv<bool>(cvars::kRtShadows, true, "Ray traced shadows (replaces shadow maps)", RT::RayTracing, {true, true, true, true});
    n += makeLv<bool>(cvars::kRtReflections, true, "Ray traced reflections", RT::RayTracing, {false, true, true, true});
    n += makeLv<bool>(cvars::kRtAO, true, "Ray traced ambient occlusion", RT::RayTracing, {false, true, true, true});
    n += makeLv<bool>(cvars::kRtGI, true, "Ray traced diffuse GI (DDGI probe volume)", RT::RayTracing, {false, false, true, true});
    n += makeLv<bool>(cvars::kRtTranslucency, true, "Ray traced refraction / glass", RT::RayTracing, {false, true, true, true});
    n += makeLv<int>(cvars::kAntiAliasing, 0, "Anti-aliasing method (implemented by features)", S::AntiAliasing, {0, 1, 2, 2});
    n += make<int>(cvars::kUpscaler, 0, "Upscaler", CVarEnum{"Off", "FSR1", "DLSS", "TAAU"}, P);
    n += make<int>(cvars::kUpscalerQuality, 3, "Upscaler quality mode",
                   CVarEnum{"UltraPerformance", "Performance", "Balanced", "Quality", "Native"}.alias("DLAA", 4), P);
    n += make<float>(cvars::kUpscalerSharpness, 0.2f, "Upscaler sharpening: FSR 1 RCAS / TAAU CAS / DLSS", 0.0f, 1.0f);
    n += make<int>(cvars::kTonemapper, 0, "Tonemapping operator", CVarEnum{"ACES", "AgX", "Neutral", "Linear"}, P);
    n += make<float>(cvars::kExposure, 0.0f, "Exposure compensation in EV (added)", -16.0f, 16.0f);
    n += make<bool>(cvars::kAutoExposure, false, "Histogram auto exposure (eye adaptation)", P);
    n += make<bool>(cvars::kShadows, true, "Raster shadows");
    n += makeLv<int>(cvars::kAOMethod, 0, "Ambient occlusion: 0 off, 1 SSAO, 2 GTAO", S::GlobalIllumination, {1, 2, 2, 2});
    n += makeLv<bool>(cvars::kSSR, false, "Screen-space reflections", S::Reflections, {false, true, true, true});
    n += makeLv<bool>(cvars::kIrradianceVolumes, true, "Baked irradiance volumes feed IndirectDiffuse", S::GlobalIllumination,
                      {false, true, true, true});
    n += make<bool>(cvars::kVSync, true, "Vertical sync", P);
    n += make<int>(cvars::kMaxFps, 0, "Frame rate limit (0 = unlimited)", 0, 1000, P);
    return n;
}

QString cvarValueLabel(const QString& cvarName, const QString& value) {
    static const QHash<QString, QString> labels = {
        {"FSR1", "AMD FSR 1.0"},
        {"DLSS", "NVIDIA DLSS"},
        {"UltraPerformance", "Ultra Performance"},
        {"Native", "DLAA / Native"},
        {"DLAA", "DLAA / Native"},
        {"TAA", "Temporal AA (TAA)"},
        {"FXAA", "FXAA"},
        {"ACES", "ACES Filmic"},
        {"AgX", "AgX"},
    };
    Q_UNUSED(cvarName);
    auto it = labels.find(value);
    return it == labels.end() ? prettifyName(value.toStdString()) : *it;
}

} // namespace ox::editor
