// CVars of the postprocess-upscalers area. Effects that change the image are off by default in code (a bare
// Renderer, tests) and switched on by the scalability levels (sg.PostProcess / sg.AntiAliasing) that the runtime and
// editor apply, or per scene by post-process volumes. r.Upscaler / r.Upscaler.Quality / r.AntiAliasing belong to
// render core (settings.cpp).
#include "pp_common.hpp"

namespace ox::render::pp {

using S = Scalability;

// --- anti-aliasing (sg.AntiAliasing) ---
CVar<int> cvTaaQuality("r.TAA.Quality", 2,
                       "TAA quality: 0 min/max clamp, 1 variance clip, 2 + Catmull-Rom history & 3x3 dilation, "
                       "3 + wider anti-flicker",
                       S::AntiAliasing, {0, 1, 2, 3});
CVar<int> cvTaaSamples("r.AntiAliasing.Samples", 8, "Temporal jitter sequence length (TAA at native resolution)",
                       S::AntiAliasing, {4, 8, 8, 16});
CVar<float> cvTaaSharpness("r.TAA.Sharpness", 0.25f, "Contrast adaptive sharpening after TAA (0..1)", 0.0f, 1.0f);
CVar<float> cvTaaCurrentWeight("r.TAA.CurrentFrameWeight", 0.08f,
                               "Minimum weight of the current frame in the TAA blend (lower = smoother, more ghosting)",
                               0.01f, 1.0f);
CVar<bool> cvTaaAntiFlicker("r.TAA.AntiFlicker", true, "Luminance weighted blending and flicker damping in TAA");
CVar<int> cvFxaaQuality("r.FXAA.Quality", 2, "FXAA edge search length: 0 short (4 steps) .. 3 long (16 steps)",
                        S::AntiAliasing, {0, 1, 2, 3});
CVar<float> cvUpscalerSharpness("r.Upscaler.Sharpness", 0.2f,
                                "Upscaler sharpening: FSR 1 RCAS / TAAU CAS / DLSS (deprecated in DLSS 3.7+)", 0.0f,
                                1.0f, CVarFlags::Persist);

// --- post processing (sg.PostProcess) ---
CVar<bool> cvBloom("r.Bloom", false, "Physically based bloom (dual filter, energy conserving)", S::PostProcess,
                   {true, true, true, true});
CVar<int> cvBloomQuality("r.Bloom.Quality", 3, "Bloom quality: mip chain length 4 + quality (wider, smoother glow)",
                         S::PostProcess, {1, 2, 3, 4});
CVar<float> cvBloomIntensity("r.Bloom.Intensity", 0.04f, "Default bloom intensity (fraction of the blurred image)",
                             0.0f, 1.0f);
CVar<bool> cvDof("r.DepthOfField", false, "Physical camera depth of field (when a volume sets a focus distance)",
                 S::PostProcess, {false, true, true, true});
CVar<int> cvDofQuality("r.DOF.Quality", 2, "Depth of field gather rings: 2 + quality", S::PostProcess, {0, 1, 2, 3});
CVar<bool> cvMotionBlur("r.MotionBlur", false, "Per-object and camera motion blur (McGuire reconstruction)",
                        S::PostProcess, {false, true, true, true});
CVar<int> cvMotionBlurQuality("r.MotionBlur.Quality", 2, "Motion blur samples: 4 + 4 × quality", S::PostProcess,
                              {0, 1, 2, 3});
CVar<bool> cvVignette("r.Vignette", true, "Lens vignette (intensity from post-process volumes)", S::PostProcess,
                      {true, true, true, true});
CVar<bool> cvChromaticAberration("r.ChromaticAberration", true,
                                 "Lateral chromatic aberration (intensity from post-process volumes)", S::PostProcess,
                                 {false, true, true, true});
CVar<bool> cvFilmGrain("r.FilmGrain", true, "Film grain (intensity from post-process volumes)", S::PostProcess,
                       {false, true, true, true});

// --- exposure / grading / sharpening (artistic, no scalability) ---
CVar<bool> cvAutoExposure("r.Exposure.Auto", false, "Histogram auto exposure (eye adaptation)", CVarFlags::Persist);
CVar<float> cvExposureMinEV("r.Exposure.MinEV100", -4.0f, "Auto exposure: darkest metered EV100", -10.0f, 24.0f);
CVar<float> cvExposureMaxEV("r.Exposure.MaxEV100", 20.0f, "Auto exposure: brightest metered EV100", -10.0f, 24.0f);
CVar<float> cvExposureSpeedUp("r.Exposure.SpeedUp", 3.0f, "Auto exposure adaptation speed towards brighter (EV/s)",
                              0.01f, 100.0f);
CVar<float> cvExposureSpeedDown("r.Exposure.SpeedDown", 1.0f, "Auto exposure adaptation speed towards darker (EV/s)",
                                0.01f, 100.0f);
CVar<float> cvSharpen("r.Sharpen", 0.0f, "Contrast adaptive sharpening of the output (0..1)", 0.0f, 1.0f);
CVar<bool> cvColorGrading("r.ColorGrading", true, "White balance / grading LUT and user LUTs from post-process volumes");

void registerCVars() { (void)cvBloom.get(); }

} // namespace ox::render::pp
