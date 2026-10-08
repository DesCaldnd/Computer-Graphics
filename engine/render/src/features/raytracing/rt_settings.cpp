// CVars of the ray tracing area (Scalability::RayTracing, Low / Medium / High / Ultra) and availability reporting.
#include <oxwald/core/cvar.hpp>
#include <oxwald/render/features/raytracing/raytracing.hpp>
#include <oxwald/render/render_settings.hpp>

#include <algorithm>
#include <format>

namespace ox::render::rt {

namespace {

using S = Scalability;
constexpr S RT = S::RayTracing;

// --- effects on/off (only effective with r.RayTracing + a capable GPU) ---
CVar<bool> cvShadows("r.RayTracing.Shadows", true, "Ray traced shadows (replaces shadow maps)", RT, {true, true, true, true});
CVar<bool> cvReflections("r.RayTracing.Reflections", true, "Ray traced reflections", RT, {false, true, true, true});
CVar<bool> cvAO("r.RayTracing.AO", true, "Ray traced ambient occlusion", RT, {false, true, true, true});
CVar<bool> cvGI("r.RayTracing.GI", true, "Ray traced diffuse GI (DDGI probe volume)", RT, {false, false, true, true});
CVar<bool> cvTranslucency("r.RayTracing.Translucency", true, "Ray traced refraction / glass", RT, {false, true, true, true});
CVar<bool> cvVolumetrics("r.RayTracing.Volumetrics", true, "Ray traced froxel light visibility", RT,
                         {false, false, true, true});

// --- shadows ---
CVar<int> cvShadowSpp("r.RayTracing.Shadows.SamplesPerPixel", 1, "Shadow rays per pixel and light", RT, {1, 1, 1, 2});
CVar<int> cvShadowLocal("r.RayTracing.Shadows.MaxLocalLights", 4,
                        "Shadowed local lights with their own visibility channel (0-4, ReSTIR handles any number)", RT,
                        {1, 2, 4, 4});
CVar<bool> cvShadowReSTIR("r.RayTracing.Shadows.ReSTIR", false,
                          "ReSTIR DI ratio estimator: ray traced shadows for every shadowed local light", RT,
                          {false, false, false, true});
CVar<bool> cvShadowColored("r.RayTracing.Shadows.Colored", true, "Coloured transmission shadows of translucent casters",
                           RT, {false, true, true, true});
CVar<bool> cvShadowDenoiser("r.RayTracing.Shadows.Denoiser", true, "Denoise ray traced shadows");
CVar<int> cvShadowScale("r.RayTracing.Shadows.ResolutionScale", 100, "Shadow ray resolution in % (50 = half + upsample)",
                        RT, {50, 100, 100, 100});

// --- reflections ---
CVar<float> cvReflMaxRough("r.RayTracing.Reflections.MaxRoughness", 0.6f,
                           "Above this roughness reflections fall back to probes / IBL", RT, {0.3f, 0.4f, 0.6f, 0.8f});
CVar<int> cvReflSpp("r.RayTracing.Reflections.SamplesPerPixel", 1, "Reflection rays per pixel", RT, {1, 1, 1, 2});
CVar<int> cvReflScale("r.RayTracing.Reflections.ResolutionScale", 100, "Reflection resolution in % (50 or 100)", RT,
                      {50, 50, 100, 100});
CVar<bool> cvReflDenoiser("r.RayTracing.Reflections.Denoiser", true, "Denoise ray traced reflections");

// --- AO ---
CVar<float> cvAoRadius("r.RayTracing.AO.Radius", 1.0f, "RTAO ray length (m)", 0.05f, 20.0f);
CVar<int> cvAoSpp("r.RayTracing.AO.SamplesPerPixel", 1, "AO rays per pixel", RT, {1, 1, 2, 4});
CVar<int> cvAoScale("r.RayTracing.AO.ResolutionScale", 100, "AO resolution in % (50 or 100)", RT, {50, 50, 100, 100});
CVar<bool> cvAoDenoiser("r.RayTracing.AO.Denoiser", true, "Denoise RTAO");

// --- GI (DDGI) ---
CVar<int> cvGiRays("r.RayTracing.GI.RaysPerProbe", 128, "DDGI rays per probe and frame", RT, {64, 96, 128, 256});
CVar<int> cvGiXZ("r.RayTracing.GI.ProbesXZ", 24, "DDGI probes along X and Z", RT, {12, 16, 24, 32});
CVar<int> cvGiY("r.RayTracing.GI.ProbesY", 8, "DDGI probes along Y", RT, {6, 8, 8, 12});
CVar<float> cvGiSpacing("r.RayTracing.GI.ProbeSpacing", 2.0f, "DDGI probe spacing (m)", 0.25f, 32.0f);
CVar<float> cvGiHysteresis("r.RayTracing.GI.Hysteresis", 0.97f, "DDGI temporal hysteresis", 0.0f, 0.999f);
CVar<int> cvGiScale("r.RayTracing.GI.ResolutionScale", 100, "Screen-space GI application resolution in %", RT,
                    {50, 50, 100, 100});

// --- translucency ---
CVar<int> cvTransBounces("r.RayTracing.Translucency.MaxBounces", 4, "Refraction bounces (entering + internal + exit)",
                         RT, {2, 3, 4, 6});

// --- volumetrics (grid = the Volumetrics feature's froxel grid) ---
CVar<int> cvVolLocal("r.RayTracing.Volumetrics.LocalSamples", 2, "Local light shadow rays per froxel", RT, {1, 1, 2, 4});
CVar<int> cvVolSky("r.RayTracing.Volumetrics.SkyRays", 1, "Sky visibility rays per froxel (0 = off)", RT, {0, 1, 1, 2});

// --- shared ---
CVar<int> cvDenoiserIterations("r.RayTracing.Denoiser.Iterations", 4, "À-trous filter passes (1-5)", RT, {3, 4, 4, 5});
CVar<int> cvDenoiserHistory("r.RayTracing.Denoiser.MaxHistory", 32, "Temporal accumulation length (frames)", 1, 255);
CVar<int> cvBlasLod("r.RayTracing.BLAS.LOD", -1, "BLAS LOD: -1 coarsest, else the requested LOD (clamped)", RT,
                    {-1, -1, -1, 0});
CVar<int> cvBlasBuilds("r.RayTracing.BLAS.BuildsPerFrame", 16, "BLAS builds per frame (streaming budget)", 1, 4096);

// --- path tracer ---
CVar<bool> cvPathTracing("r.PathTracing", false, "Reference path tracer (progressive accumulation, needs ray tracing)");
CVar<int> cvPathMode("r.PathTracing.Mode", 0, "Path tracer implementation", CVarEnum{"RayQuery", "Pipeline"});
CVar<int> cvPathBounces("r.PathTracing.MaxBounces", 8, "Maximum path length", 1, 64);
CVar<int> cvPathSpf("r.PathTracing.SamplesPerFrame", 1, "Samples per pixel added each frame", 1, 64);
CVar<int> cvPathMax("r.PathTracing.MaxSamples", 4096, "Stop accumulating after this many samples", 1, 1 << 20);

} // namespace

void registerRayTracingCVars() { (void)cvShadows.get(); }

RtSettings RtSettings::fromCVars() {
    RtSettings s;
    s.shadows = cvShadows.get();
    s.shadowSpp = std::clamp(cvShadowSpp.get(), 1, 16);
    s.shadowMaxLocalLights = std::clamp(cvShadowLocal.get(), 0, 4);
    s.shadowReSTIR = cvShadowReSTIR.get();
    s.shadowColored = cvShadowColored.get();
    s.shadowDenoiser = cvShadowDenoiser.get();
    s.shadowResolutionScale = cvShadowScale.get() <= 50 ? 50 : 100;
    s.reflections = cvReflections.get();
    s.reflectionMaxRoughness = std::clamp(cvReflMaxRough.get(), 0.0f, 1.0f);
    s.reflectionSpp = std::clamp(cvReflSpp.get(), 1, 16);
    s.reflectionResolutionScale = cvReflScale.get() <= 50 ? 50 : 100;
    s.reflectionDenoiser = cvReflDenoiser.get();
    s.ao = cvAO.get();
    s.aoRadius = cvAoRadius.get();
    s.aoSpp = std::clamp(cvAoSpp.get(), 1, 64);
    s.aoResolutionScale = cvAoScale.get() <= 50 ? 50 : 100;
    s.aoDenoiser = cvAoDenoiser.get();
    s.gi = cvGI.get();
    s.giRaysPerProbe = std::clamp(cvGiRays.get(), 16, 512);
    s.giProbesXZ = std::clamp(cvGiXZ.get(), 2, 64);
    s.giProbesY = std::clamp(cvGiY.get(), 2, 32);
    s.giProbeSpacing = cvGiSpacing.get();
    s.giHysteresis = cvGiHysteresis.get();
    s.giResolutionScale = cvGiScale.get() <= 50 ? 50 : 100;
    s.translucency = cvTranslucency.get();
    s.translucencyMaxBounces = std::clamp(cvTransBounces.get(), 1, 16);
    s.volumetrics = cvVolumetrics.get();
    s.volumetricLocalSamples = std::clamp(cvVolLocal.get(), 0, 16);
    s.volumetricSkyRays = std::clamp(cvVolSky.get(), 0, 16);
    s.denoiserIterations = std::clamp(cvDenoiserIterations.get(), 0, 5);
    s.denoiserMaxHistory = cvDenoiserHistory.get();
    s.blasLod = cvBlasLod.get();
    s.blasBuildsPerFrame = cvBlasBuilds.get();
    s.pathTracing = cvPathTracing.get();
    s.pathTracingMode = cvPathMode.get();
    s.pathMaxBounces = cvPathBounces.get();
    s.pathSamplesPerFrame = cvPathSpf.get();
    s.pathMaxSamples = cvPathMax.get();
    return s;
}

bool rayTracingActive(const RenderSettings& settings, const rhi::DeviceCaps& caps) {
    return settings.rayTracing && caps.rayTracingSupported();
}

RtStatus rayTracingStatus(const rhi::DeviceCaps& caps) {
    RtStatus st;
    st.available = caps.rayTracingSupported();
    st.reason = caps.whyRayTracingUnavailable();
    struct E {
        const char* name;
        const char* cvar;
    };
    for (const E e : {E{"Shadows", "r.RayTracing.Shadows"}, E{"Reflections", "r.RayTracing.Reflections"},
                      E{"Ambient occlusion", "r.RayTracing.AO"}, E{"Global illumination", "r.RayTracing.GI"},
                      E{"Translucency", "r.RayTracing.Translucency"}, E{"Volumetrics", "r.RayTracing.Volumetrics"},
                      E{"ReSTIR DI", "r.RayTracing.Shadows.ReSTIR"}, E{"Path tracer (ray query)", "r.PathTracing"}}) {
        RtEffectStatus es;
        es.name = e.name;
        es.cvar = e.cvar;
        es.available = st.available;
        es.reason = st.reason;
        st.effects.push_back(std::move(es));
    }
    RtEffectStatus pipe;
    pipe.name = "Path tracer (RT pipeline)";
    pipe.cvar = "r.PathTracing.Mode";
    pipe.available = st.available && caps.rayTracingPipeline;
    if (!st.available) pipe.reason = st.reason;
    else if (!caps.rayTracingPipeline) {
        pipe.reason = std::format("{} does not expose VK_KHR_ray_tracing_pipeline (the ray query mode still works)",
                                  caps.gpuName.empty() ? "The GPU" : caps.gpuName);
    }
    st.effects.push_back(std::move(pipe));
    return st;
}

std::vector<std::string> rayTracingCVarNames() {
    std::vector<std::string> out;
    for (ICVar* c : CVarRegistry::instance().all()) {
        const std::string_view n = c->name();
        if (n.starts_with("r.RayTracing.") || n.starts_with("r.PathTracing")) out.emplace_back(n);
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace ox::render::rt
