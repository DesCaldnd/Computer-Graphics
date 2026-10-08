#include <oxwald/core/cvar.hpp>
#include <oxwald/core/hash.hpp>
#include <oxwald/render/render_settings.hpp>

namespace ox::render {

namespace {

using S = Scalability;

// --- resolution / AA / upscaling hooks ---
CVar<float> cvScreenPercentage("r.ScreenPercentage", 100.0f, "Render resolution in percent of the output resolution",
                               25.0f, 200.0f, CVarFlags::Persist);
CVar<int> cvAntiAliasing("r.AntiAliasing", 0, "Anti-aliasing method (implemented by features)",
                         S::AntiAliasing, {0, 1, 2, 2});
CVar<int> cvUpscaler("r.Upscaler", 0, "Upscaler", CVarEnum{"Off", "FSR1", "DLSS"}, CVarFlags::Persist);
CVar<int> cvUpscalerQuality("r.Upscaler.Quality", 3, "Upscaler quality mode",
                            CVarEnum{"UltraPerformance", "Performance", "Balanced", "Quality", "Native"},
                            CVarFlags::Persist);
CVar<bool> cvRayTracing("r.RayTracing", false, "Ray traced effects (ignored when the GPU lacks ray queries)",
                        CVarFlags::Persist);

// --- shading ---
CVar<int> cvTonemapper("r.Tonemapper", 0, "Tonemapping operator", CVarEnum{"ACES", "AgX", "Neutral", "Linear"},
                       CVarFlags::Persist);
CVar<int> cvExposureMode("r.Exposure.Mode", 0, "Exposure source", CVarEnum{"Camera", "Manual"});
CVar<float> cvExposureEV100("r.Exposure.EV100", 10.0f, "Manual exposure (EV100)", -10.0f, 24.0f);
CVar<float> cvExposureCompensation("r.Exposure.Compensation", 0.0f, "Exposure compensation in EV (added)", -16.0f,
                                   16.0f);
CVar<bool> cvIbl("r.IBL", true, "Image based lighting from the environment");
CVar<float> cvIblIntensity("r.IBL.Intensity", 1.0f, "IBL intensity multiplier", 0.0f, 16.0f);
CVar<int> cvIblResolution("r.IBL.Resolution", 128, "Prefiltered environment cube face size", S::Shading,
                          {64, 128, 128, 256});
CVar<bool> cvMultiScatter("r.Shading.MultiScatter", true, "GGX multiple scattering energy compensation", S::Shading,
                          {false, true, true, true});
CVar<int> cvMaxLights("r.Lights.Max", 4096, "Maximum local lights uploaded per view", 0, 65536);
CVar<int> cvClusterMaxLights("r.Clusters.MaxLightsPerCluster", 256, "Light list capacity per cluster", 16, 1024);
CVar<float> cvClusterMaxDistance("r.Clusters.MaxDistance", 500.0f, "Far bound of the cluster grid (m)", 10.0f,
                                 100000.0f);
CVar<bool> cvSky("r.Sky", true, "Draw the sky");

// --- shadows ---
CVar<bool> cvShadows("r.Shadows", true, "Raster shadows");
CVar<int> cvCsmResolution("r.Shadows.CSM.Resolution", 2048, "Cascade resolution", S::Shadows, {1024, 2048, 2048, 4096});
CVar<int> cvCsmCascades("r.Shadows.CSM.Cascades", 4, "Cascade count", S::Shadows, {2, 3, 4, 4});
CVar<float> cvCsmDistance("r.Shadows.CSM.Distance", 120.0f, "Directional shadow distance (m)", S::Shadows,
                          {60.0f, 100.0f, 150.0f, 250.0f});
CVar<float> cvCsmLambda("r.Shadows.CSM.Lambda", 0.75f, "Cascade split blend: 0 uniform, 1 logarithmic", 0.0f, 1.0f);
CVar<float> cvCsmBlend("r.Shadows.CSM.Blend", 0.1f, "Cascade transition blend fraction", 0.0f, 0.5f);
CVar<int> cvAtlasSize("r.Shadows.AtlasSize", 4096, "Spot light shadow atlas size", S::Shadows, {2048, 4096, 4096, 8192});
CVar<int> cvPointResolution("r.Shadows.PointResolution", 512, "Point light cube face size", S::Shadows,
                            {256, 512, 512, 1024});
CVar<int> cvMaxShadowed("r.Shadows.MaxShadowedLights", 16, "Maximum shadowed local lights", S::Shadows, {4, 8, 16, 32});
CVar<int> cvMaxPointShadows("r.Shadows.MaxPointShadows", 8, "Maximum shadowed point lights", S::Shadows, {2, 4, 8, 12});
CVar<int> cvPcfTaps("r.Shadows.PCFTaps", 16, "Poisson PCF taps (0 = one hardware compare)", S::Shadows, {0, 8, 16, 16});
CVar<bool> cvPcss("r.Shadows.PCSS", true, "Percentage-closer soft shadows (contact hardening)", S::Shadows,
                  {false, false, true, true});
CVar<float> cvFilterRadius("r.Shadows.FilterRadius", 1.5f, "PCF radius in texels", 0.0f, 16.0f);
CVar<int> cvSpotMin("r.Shadows.SpotMinResolution", 128, "Smallest spot shadow tile", 32, 4096);
CVar<int> cvSpotMax("r.Shadows.SpotMaxResolution", 1024, "Largest spot shadow tile", S::Shadows, {512, 1024, 1024, 2048});
CVar<bool> cvShadowCaching("r.Shadows.Caching", true, "Re-render local light shadows only when something moved");

// --- textures ---
CVar<int> cvAnisotropy("r.Textures.Anisotropy", 8, "Maximum anisotropic filtering", S::Textures, {2, 4, 8, 16});
CVar<float> cvMipBias("r.Textures.MipBias", 0.0f, "Material texture mip bias", S::Textures, {1.0f, 0.5f, 0.0f, 0.0f});
CVar<int> cvMaxTextureSize("r.Textures.MaxSize", 8192, "Largest uploaded texture dimension (larger mips skipped)",
                           S::Textures, {1024, 2048, 4096, 8192});

// --- view distance ---
CVar<float> cvDrawDistance("r.ViewDistance.DrawDistance", 0.0f, "Instance draw distance in m (0 = camera far plane)",
                           S::ViewDistance, {400.0f, 1000.0f, 2500.0f, 0.0f});
CVar<float> cvLodBias("r.ViewDistance.LODBias", 0.0f, "Mesh LOD bias (positive = coarser)", S::ViewDistance,
                      {1.0f, 0.5f, 0.0f, -0.5f});

// --- debug ---
CVar<int> cvDebugView("r.DebugView", 0, "Debug visualisation",
                      CVarEnum{"None", "Albedo", "Normals", "Roughness", "Metallic", "AO", "Emissive", "LightComplexity",
                               "Overdraw", "ShadowCascades", "Wireframe", "Velocity", "Depth", "ShadowMask"});
CVar<bool> cvWireframe("r.Wireframe", false, "Wireframe overlay");
CVar<bool> cvGpuTimings("r.GpuTimings", true, "Per-pass GPU timestamps");
CVar<bool> cvFrustumCulling("r.FrustumCulling", true, "CPU frustum culling of instances");

} // namespace

void registerRenderCVars() {
    // The cvars above are file statics of this translation unit; referencing this function from Renderer::create
    // keeps the object file (and therefore their registration) in the link.
    (void)cvScreenPercentage.get();
}

const char* debugViewName(DebugView v) {
    static constexpr const char* kNames[] = {"None",     "Albedo",          "Normals",  "Roughness",      "Metallic",
                                             "AO",       "Emissive",        "LightComplexity", "Overdraw", "ShadowCascades",
                                             "Wireframe", "Velocity",       "Depth",    "ShadowMask"};
    const auto i = static_cast<usize>(v);
    return i < std::size(kNames) ? kNames[i] : "?";
}

RenderSettings RenderSettings::fromCVars() {
    RenderSettings s;
    s.screenPercentage = cvScreenPercentage.get();
    s.tonemapper = Tonemapper(cvTonemapper.get());
    s.exposureMode = ExposureMode(cvExposureMode.get());
    s.manualEV100 = cvExposureEV100.get();
    s.exposureCompensation = cvExposureCompensation.get();
    s.ibl = cvIbl.get();
    s.iblIntensity = cvIblIntensity.get();
    s.iblResolution = cvIblResolution.get();
    s.multiScatter = cvMultiScatter.get();
    s.maxLights = cvMaxLights.get();
    s.clusterMaxLights = cvClusterMaxLights.get();
    s.clusterMaxDistance = cvClusterMaxDistance.get();
    s.sky = cvSky.get();
    s.shadows = cvShadows.get();
    s.csmResolution = cvCsmResolution.get();
    s.csmCascades = std::clamp(cvCsmCascades.get(), 1, 4);
    s.csmDistance = cvCsmDistance.get();
    s.csmLambda = cvCsmLambda.get();
    s.csmBlend = cvCsmBlend.get();
    s.atlasSize = cvAtlasSize.get();
    s.pointResolution = cvPointResolution.get();
    s.maxShadowedLights = cvMaxShadowed.get();
    s.maxPointShadows = cvMaxPointShadows.get();
    s.pcfTaps = std::clamp(cvPcfTaps.get(), 0, 16);
    s.pcss = cvPcss.get();
    s.shadowFilterRadius = cvFilterRadius.get();
    s.spotMinResolution = cvSpotMin.get();
    s.spotMaxResolution = cvSpotMax.get();
    s.shadowCaching = cvShadowCaching.get();
    s.anisotropy = cvAnisotropy.get();
    s.mipBias = cvMipBias.get();
    s.maxTextureSize = cvMaxTextureSize.get();
    s.drawDistance = cvDrawDistance.get();
    s.lodBias = cvLodBias.get();
    s.antiAliasing = cvAntiAliasing.get();
    s.upscaler = cvUpscaler.get();
    s.upscalerQuality = cvUpscalerQuality.get();
    s.rayTracing = cvRayTracing.get();
    s.debugView = DebugView(std::clamp(cvDebugView.get(), 0, int(DebugView::Count) - 1));
    s.wireframe = cvWireframe.get();
    s.gpuTimings = cvGpuTimings.get();
    s.frustumCulling = cvFrustumCulling.get();
    return s;
}

u64 RenderSettings::topologyHash() const {
    u64 h = 0xcbf29ce484222325ull;
    auto mix = [&](const auto& v) { h = hashCombine(h, std::hash<std::decay_t<decltype(v)>>{}(v)); };
    mix(screenPercentage), mix(int(tonemapper)), mix(ibl), mix(iblResolution), mix(shadows), mix(csmResolution);
    mix(csmCascades), mix(atlasSize), mix(pointResolution), mix(maxPointShadows), mix(antiAliasing), mix(upscaler);
    mix(upscalerQuality), mix(rayTracing), mix(int(debugView)), mix(wireframe), mix(sky), mix(clusterMaxLights);
    return h;
}

} // namespace ox::render
