// CVars of the reflections-ao area. Scalability tables are Low / Medium / High / Ultra.
//
// The master switches of the screen-space effects (r.SSR, r.AO.Method) are declared off so headless tools and
// tests that never pick a quality level render exactly as before; every quality preset (the runtime and editor
// always apply one) turns them on from Medium (SSR) / Low (AO).
#include "reflection_internal.hpp"

namespace ox::render::reflections::cv {

using S = Scalability;

// --- Reflections ---
CVar<bool> probes("r.ReflectionProbes", true, "Local reflection probes (cube captures, box projection, blending)");
CVar<int> probeResolution("r.ReflectionProbes.Resolution", 128, "Maximum reflection probe cube face size",
                          S::Reflections, {64, 128, 128, 256});
CVar<bool> probeRealtime("r.ReflectionProbes.Realtime", true,
                         "Update Realtime reflection probes continuously (off: captured once like OnEnable)",
                         S::Reflections, {false, true, true, true});
CVar<int> probeRealtimeFaces("r.ReflectionProbes.RealtimeFacesPerFrame", 1,
                             "Cube faces re-captured per frame for each Realtime probe", S::Reflections, {1, 1, 1, 2});
CVar<int> probeMaxPerPixel("r.ReflectionProbes.MaxPerPixel", 4, "Overlapping probes blended per pixel", S::Reflections,
                           {1, 2, 4, 4});
CVar<int> probeCapturesPerFrame("r.ReflectionProbes.CapturesPerFrame", 2,
                                "Full probe captures per frame (bake / OnEnable budget)", 1, 64);
CVar<bool> ssr("r.SSR", false, "Screen-space reflections (Hi-Z ray march, stochastic GGX, temporal)", S::Reflections,
               {false, true, true, true});
CVar<int> ssrQuality("r.SSR.Quality", 2, "SSR quality: spatial reuse samples 1/4/4/8, temporal blend", S::Reflections,
                     {0, 1, 2, 3});
CVar<int> ssrMaxSteps("r.SSR.MaxSteps", 64, "Hi-Z ray march iterations", S::Reflections, {24, 40, 64, 96});
CVar<bool> ssrHalfRes("r.SSR.HalfRes", false, "Trace SSR rays at half resolution", S::Reflections,
                      {true, true, false, false});
CVar<float> ssrMaxRoughness("r.SSR.MaxRoughness", 0.7f, "Rougher surfaces use probes only", S::Reflections,
                            {0.35f, 0.5f, 0.7f, 0.85f});
CVar<float> ssrThickness("r.SSR.Thickness", 0.3f, "Assumed depth-buffer thickness for SSR hits (m, grows with distance)",
                         0.01f, 10.0f);
CVar<bool> ssrTemporal("r.SSR.Temporal", true, "Temporal accumulation of SSR");
CVar<bool> planar("r.PlanarReflections", true, "Planar reflections (PlanarReflector components)", S::Reflections,
                  {false, true, true, true});
CVar<float> planarScale("r.PlanarReflections.ResolutionScale", 0.75f,
                        "Planar reflection resolution relative to the render resolution", S::Reflections,
                        {0.25f, 0.5f, 0.75f, 1.0f});
CVar<int> planarMax("r.PlanarReflections.MaxReflectors", 2, "Planar reflectors rendered per view", 0, 4);
CVar<int> captureMaxLocalLights("r.Reflections.CaptureMaxLocalLights", 64,
                                "Local lights evaluated in probe / planar captures", 0, 1024);

// --- GlobalIllumination ---
CVar<int> aoMethod("r.AO.Method", 0, "Ambient occlusion: 0 off, 1 SSAO, 2 GTAO", S::GlobalIllumination, {1, 2, 2, 2});
CVar<int> aoQuality("r.AO.Quality", 2, "AO quality: GTAO slices × steps 1×4, 2×6, 2×8, 3×12 (SSAO: 8..24 samples)",
                    S::GlobalIllumination, {0, 1, 2, 3});
CVar<bool> aoHalfRes("r.AO.HalfRes", false, "Compute AO at half resolution (bilateral upsample)", S::GlobalIllumination,
                     {true, true, false, false});
CVar<float> aoRadius("r.AO.Radius", 0.75f, "AO world-space radius (m)", 0.05f, 10.0f);
CVar<float> aoIntensity("r.AO.Intensity", 1.0f, "AO strength (visibility exponent)", 0.0f, 4.0f);
CVar<bool> aoTemporal("r.AO.Temporal", true, "Temporal accumulation of AO");
CVar<bool> giVolumes("r.GI.IrradianceVolumes", true, "Baked irradiance volumes feed IndirectDiffuse",
                     S::GlobalIllumination, {false, true, true, true});
CVar<int> giProbesPerFrame("r.GI.IrradianceVolumes.ProbesPerFrame", 32, "Irradiance probes baked per frame", 1, 4096);

std::vector<std::string> reflectionNames() {
    return {"r.ReflectionProbes", "r.ReflectionProbes.Resolution", "r.ReflectionProbes.Realtime",
            "r.ReflectionProbes.RealtimeFacesPerFrame", "r.ReflectionProbes.MaxPerPixel",
            "r.ReflectionProbes.CapturesPerFrame", "r.SSR", "r.SSR.Quality", "r.SSR.MaxSteps", "r.SSR.HalfRes",
            "r.SSR.MaxRoughness", "r.SSR.Thickness", "r.SSR.Temporal", "r.PlanarReflections",
            "r.PlanarReflections.ResolutionScale", "r.PlanarReflections.MaxReflectors",
            "r.Reflections.CaptureMaxLocalLights"};
}
std::vector<std::string> aoNames() {
    return {"r.AO.Method", "r.AO.Quality", "r.AO.HalfRes", "r.AO.Radius", "r.AO.Intensity", "r.AO.Temporal"};
}
std::vector<std::string> giNames() { return {"r.GI.IrradianceVolumes", "r.GI.IrradianceVolumes.ProbesPerFrame"}; }

} // namespace ox::render::reflections::cv

namespace ox::render {
void registerReflectionCVars() { (void)reflections::cv::probes.get(); }
} // namespace ox::render
