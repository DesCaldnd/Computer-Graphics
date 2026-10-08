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
    int n = 0;
    // ---- feature switches (Project Settings > Rendering) ----
    n += make<bool>(cvars::kRayTracing, false, "Hardware ray tracing (VK_KHR_ray_query); only settable when the GPU supports it", P);
    n += make<bool>(cvars::kRtShadows, true, "Ray traced shadows (when r.RayTracing is on)", P);
    n += make<bool>(cvars::kRtReflections, true, "Ray traced reflections", P);
    n += make<bool>(cvars::kRtAO, true, "Ray traced ambient occlusion", P);
    n += make<bool>(cvars::kRtGI, false, "Ray traced global illumination", P);
    n += make<bool>(cvars::kRtTranslucency, false, "Ray traced translucency/refraction", P);
    n += make<int>(cvars::kAntiAliasing, 2, "Anti-aliasing method", CVarEnum{"None", "FXAA", "TAA"}, P);
    n += make<int>(cvars::kUpscaler, 0, "Temporal upscaler", CVarEnum{"Off", "FSR1", "DLSS", "TAAU"}, P);
    n += make<int>(cvars::kUpscalerQuality, 3, "Upscaler quality mode",
                   CVarEnum{"UltraPerformance", "Performance", "Balanced", "Quality", "DLAA"}, P);
    n += make<float>(cvars::kUpscalerSharpness, 0.2f, "Upscaler sharpening (RCAS / DLSS sharpness)", 0.0f, 1.0f, P);
    n += make<int>(cvars::kTonemapper, 0, "Tonemapping operator", CVarEnum{"ACES", "AgX"}, P);
    n += make<float>(cvars::kExposure, 0.0f, "Default exposure compensation (EV)", -10.0f, 10.0f, P);
    n += make<bool>(cvars::kAutoExposure, true, "Eye adaptation (histogram auto exposure)", P);
    n += make<int>(cvars::kShadowMethod, 0, "Shadow technique", CVarEnum{"ShadowMaps", "RayTraced"}, P);
    n += make<int>(cvars::kGIMethod, 1, "Global illumination technique", CVarEnum{"None", "Probes", "ScreenSpace", "RayTraced"}, P);
    n += make<int>(cvars::kReflectionMethod, 1, "Reflection technique", CVarEnum{"Probes", "ScreenSpace", "RayTraced"}, P);
    n += make<bool>(cvars::kVSync, true, "Vertical sync", P);
    n += make<int>(cvars::kMaxFps, 0, "Frame rate limit (0 = unlimited)", 0, 1000, P);

    // ---- scalability-bound cvars (Low, Medium, High, Ultra) ----
    n += makeLv<float>("r.ViewDistanceScale", 1.0f, "Draw distance multiplier", S::ViewDistance, {0.4f, 0.6f, 0.8f, 1.0f});
    n += makeLv<float>("r.LOD.DistanceScale", 1.0f, "Mesh LOD switch distance multiplier", S::ViewDistance, {0.5f, 0.75f, 1.0f, 1.5f});
    n += makeLv<int>("r.TAA.Quality", 2, "TAA quality (history samples / clamping)", S::AntiAliasing, {0, 1, 2, 3});
    n += makeLv<int>("r.AntiAliasing.Samples", 8, "Temporal jitter sequence length", S::AntiAliasing, {4, 8, 8, 16});
    n += makeLv<int>("r.Shadows.Resolution", 2048, "Shadow map resolution", S::Shadows, {512, 1024, 2048, 4096});
    n += makeLv<int>("r.Shadows.CascadeCount", 3, "Directional light cascades", S::Shadows, {1, 2, 3, 4});
    n += makeLv<float>("r.Shadows.Distance", 150.0f, "Shadow draw distance (m)", S::Shadows, {40.0f, 80.0f, 150.0f, 300.0f});
    n += makeLv<int>("r.Shadows.FilterQuality", 2, "PCF/PCSS filter quality", S::Shadows, {0, 1, 2, 3});
    n += makeLv<int>("r.GI.Quality", 2, "Indirect lighting quality", S::GlobalIllumination, {0, 1, 2, 3});
    n += makeLv<int>("r.SSAO.Quality", 2, "Ambient occlusion quality", S::GlobalIllumination, {0, 1, 2, 3});
    n += makeLv<int>("r.SSR.Quality", 2, "Screen space reflection quality", S::Reflections, {0, 1, 2, 3});
    n += makeLv<int>("r.ReflectionProbes.Resolution", 256, "Reflection probe cube size", S::Reflections, {64, 128, 256, 512});
    n += makeLv<int>("r.Bloom.Quality", 3, "Bloom mip chain quality", S::PostProcess, {1, 2, 3, 4});
    n += makeLv<int>("r.DOF.Quality", 2, "Depth of field quality", S::PostProcess, {0, 1, 2, 3});
    n += makeLv<int>("r.MotionBlur.Quality", 2, "Motion blur quality", S::PostProcess, {0, 1, 2, 3});
    n += makeLv<int>("r.Textures.MaxAnisotropy", 8, "Anisotropic filtering", S::Textures, {2, 4, 8, 16});
    n += makeLv<int>("r.Textures.PoolSizeMB", 2048, "Texture streaming pool (MB)", S::Textures, {512, 1024, 2048, 4096});
    n += makeLv<float>("r.Textures.MipBias", 0.0f, "Texture LOD bias", S::Textures, {1.0f, 0.5f, 0.0f, 0.0f});
    n += makeLv<int>("r.Particles.MaxCount", 40000, "GPU particle budget", S::Effects, {2000, 10000, 40000, 100000});
    n += makeLv<int>("r.Particles.LightingQuality", 1, "Particle lighting", S::Effects, {0, 0, 1, 2});
    n += makeLv<float>("r.Foliage.Density", 1.0f, "Foliage instance density", S::Foliage, {0.25f, 0.5f, 0.75f, 1.0f});
    n += makeLv<float>("r.Foliage.DrawDistance", 200.0f, "Foliage draw distance (m)", S::Foliage, {50.0f, 100.0f, 200.0f, 400.0f});
    n += makeLv<int>("r.Shading.Quality", 2, "Material/shading model quality", S::Shading, {0, 1, 2, 3});
    n += makeLv<int>("r.Shading.MaxLightsPerCluster", 64, "Clustered lights per cluster", S::Shading, {16, 32, 64, 128});
    n += makeLv<bool>("r.Volumetrics.Enable", true, "Volumetric fog and light shafts", S::Volumetrics, {false, true, true, true});
    n += makeLv<int>("r.Volumetrics.GridResolution", 96, "Froxel grid depth slices", S::Volumetrics, {32, 64, 96, 128});
    n += makeLv<int>("r.RayTracing.SamplesPerPixel", 1, "Rays per pixel for RT effects", S::RayTracing, {1, 1, 2, 4});
    n += makeLv<float>("r.RayTracing.MaxRoughness", 0.6f, "Roughness cut-off for RT reflections", S::RayTracing, {0.3f, 0.5f, 0.6f, 0.8f});
    n += makeLv<int>("r.RayTracing.Denoiser", 1, "Denoiser quality", S::RayTracing, {0, 1, 1, 2});
    return n;
}

QString cvarValueLabel(const QString& cvarName, const QString& value) {
    static const QHash<QString, QString> labels = {
        {"FSR1", "AMD FSR 1.0"},
        {"DLSS", "NVIDIA DLSS"},
        {"UltraPerformance", "Ultra Performance"},
        {"DLAA", "DLAA / Native"},
        {"ShadowMaps", "Cascaded Shadow Maps"},
        {"RayTraced", "Ray Traced"},
        {"ScreenSpace", "Screen Space"},
        {"Probes", "Light/Reflection Probes"},
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
