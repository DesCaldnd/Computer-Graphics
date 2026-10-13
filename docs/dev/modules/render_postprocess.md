# render — post-processing, anti-aliasing & upscalers (area `postprocess-upscalers`)

Code: `engine/render/src/features/postprocess/` (+ `upscalers/`), shaders `engine/shaders/render/postprocess/`,
public API `<oxwald/render/features/postprocess/postprocess.hpp>`, component
`<oxwald/render/components/postprocess.hpp>`. Registered by `registerPostProcessFeatures(f)` (marker line in
`registerBuiltinFeatures`) and `registerPostProcessTypes()` (from `registerRenderTypes()`).

## Frame chain

| Point | Feature (order) | What |
| --- | --- | --- |
| PreDepth | AutoExposure | pre-exposure hook: `VIEW.preExposure` = GPU exposure of 3 frames ago (host-visible ring) |
| BeforePostProcess | AutoExposure (-100) | 128-bin centre-weighted log-luminance histogram → average between the 50th and 90th percentile (volume-overridable) → middle-grey calibration (the metered average lands on 18 %, −0.79 EV vs the plain K = 12.5 / saturation formulas) → EV100 adaptation → `Exposure` (R32F 1×1) |
| BeforePostProcess | TAA \| TAAU \| DLSS (0) | TAA at render res; temporal upscalers output **output resolution** here, so post runs at output res |
| PostProcess | DepthOfField (200), MotionBlur (300), Bloom (400) | work at whatever resolution `SceneColorHDR` has; Depth/Velocity sampled by uv |
| Upscale | FSR1 (EASU + RCAS) | spatial, after post at render res; TAAU/DLSS only reserve the slot (no bilinear resample) |
| AfterUpscale | Sharpen (50), PostComposite (100) | CAS; chromatic aberration → bloom + lens dirt → vignette → white balance / grading LUT |
| core Tonemap | | reads `Exposure` |
| Overlay | LdrPost (-100000) | FXAA, user LUT texture, film grain — before editor overlays / UI |

**Grading vs tonemapper.** Parametric grading (white balance, contrast, saturation, lift/gamma/gain) is baked into a
32³ RGBA16F 3D LUT (re-baked only when the parameters' hash changes) indexed by log2 of *exposed* scene values
(mid grey −10 … +6.5 EV) and applied in HDR before the core tonemapper, so ACES/AgX/Neutral all see the graded
image. User LUT textures (N²×N strips, e.g. 1024×32, display referred like UE) are applied after tonemapping.

**Pre-exposure.** With auto exposure, `SceneColorHDR = radiance × preExposure` where preExposure follows the adapted
exposure with 3 frames of latency, keeping FP16 well conditioned in 300 lux and 100 klx scenes alike. Temporal
features must rescale their history by `preExposure(now) / preExposure(previous)` (TAA/TAAU do; DLSS gets
`InPreExposure`). The tonemapper multiplies by `Exposure / preExposure`.

**MSAA.** The renderer is forward+ without MSAA targets today. TAA/TAAU/DLSS need a single-sample, jittered colour +
depth + velocity; if MSAA is added, resolve colour (and take the closest depth sample) before BeforePostProcess and
keep `r.AntiAliasing` at None/FXAA for MSAA-only setups — temporal AA on top of MSAA only adds blur.

## Upscalers

`r.Upscaler` (core cvar, `Off, FSR1, DLSS, TAAU` — TAAU appended by this area) + `r.Upscaler.Quality`
(`UltraPerformance` 33 %, `Performance` 50 %, `Balanced` 58 %, `Quality` 67 %, `Native` = DLAA / 100 %). Features set
`ViewSetup::screenPercentage` (overrides `r.ScreenPercentage`), `jitterPhases` (8 × (out/in)², 8…64) and
`mipBias` (`log2(render/output)`, −1 more for DLSS per NVIDIA). `r.Upscaler.Sharpness` drives RCAS / TAAU CAS; it is
passed to DLSS as `InSharpness`, which current DLSS models ignore (NGX sharpening is deprecated).

| | FSR1 | TAAU | DLSS |
| --- | --- | --- | --- |
| kind | spatial (EASU + RCAS, MIT port in GLSL) | temporal (TAA kernel with output-res history) | NGX Vulkan |
| AA | consumes TAA/FXAA at render res | built in | built in (DLAA at 100 %) |
| platforms | everywhere | everywhere | NVIDIA RTX on Windows/Linux |

`upscalerAvailability(device)` → per upscaler `{available, temporal, reason}` for the settings UI (the editor's
`rendering_caps.cpp` can replace its own DLSS heuristics with it). `r.Upscaler=DLSS` where DLSS cannot run falls back
to TAAU.

### DLSS (`upscalers/dlss_ngx.cpp`, `dlss_feature.cpp`)

Verified on Windows 11 with an NVIDIA RTX 3080 (driver 617.42, NGX 310.9.1): the conventions, resolutions and numbers
below were measured there by the `PostProcessTest.Dlss*` GPU tests. Nothing was run on Linux.

**Build.** Compiled into `ox_render` only with `TARGET NVIDIA::DLSS` and `OX_ENABLE_DLSS` (defines
`OX_RENDER_HAS_DLSS`); otherwise `dlss_stub.cpp` reports the reason and `ox_dlss_syntax_check` (OBJECT library, never
linked) compiles `dlss_ngx.cpp` against `NVIDIA::DLSSHeaders` — on macOS this keeps the NGX code compiling.
`ox_deploy_vulkan_runtime` copies the NGX runtime next to every executable: the release libraries for Release /
RelWithDebInfo, the development ones (`OX_DLSS_RUNTIME_FILES_DEBUG`, same file names, on-screen debug overlay) for
Debug. NGX gets the executable directory and the current working directory as feature paths and writes its own data
to `<temp>/oxwald_ngx`.

**Device extensions.** NGX needs the extensions `NVSDK_NGX_VULKAN_RequiredExtensions` lists (on this machine
`VK_NVX_binary_import`, `VK_NVX_image_view_handle`, `VK_KHR_push_descriptor`).
`render::appendUpscalerVulkanExtensions(DeviceDesc&)` adds them to
`rhi::DeviceDesc::optionalInstanceExtensions/optionalDeviceExtensions` and must be called before
`rhi::Device::create`; `VK_EXT_buffer_device_address` is left out because rhi enables the core 1.2 feature
(VUID-VkDeviceCreateInfo-pNext-04748). Callers: the runtime renderer, the editor viewport
(`VulkanViewportHub::setDeviceDescHook`) and the render GPU test fixture.

**Availability.** `dlss::probe(device)` (once per device) first compares the required list with what the device was
really created with (`rhi::Device::hasInstanceExtension` / `hasDeviceExtension`; optional extensions are skipped
silently when unsupported). NGX itself does not check: initialising it on a device without those extensions only
produces validation errors from its own descriptor set layouts and `NVSDK_NGX_Result_FAIL_PlatformError`. With an
extension missing, the probe does not call NGX at all and reports a reason naming `appendUpscalerVulkanExtensions`;
`r.Upscaler=DLSS` then falls back to TAAU as on any other GPU. `DlssSelectionFallsBackToTaauWhereUnavailable` checks
this on the RTX machine by recreating the device without the extensions (no validation errors). With the extensions
present: `NVSDK_NGX_VULKAN_Init_with_ProjectID` (custom engine type), then the capability parameters
(`SuperSampling.Available`, `NeedsUpdatedDriver`, `FeatureInitResult`).

**Lifetime.** NGX keeps Vulkan objects (pipelines, layouts, samplers) on the device for as long as it is initialised,
whoever probed it — a `Renderer` or just `upscalerAvailability()` / `recommendedSettings()`. It is therefore shut down
(`DestroyParameters` + `Shutdown1`) from a callback the probe registers with `rhi::Device::addShutdownCallback`, which
runs in `~Device` before `vkDestroyDevice`. `DlssFeature::shutdown` only releases the per-view features. The probe
result is forgotten with the device, so the next device is probed afresh. Before this change, probing without a
renderer left the NGX objects alive at `vkDestroyDevice` and crashed at process exit.
`DlssSurvivesRendererAndDeviceRecreation` covers a second renderer on one device, a new device and a probe-only device.

**Per-view feature.** `NGX_DLSS_GET_OPTIMAL_SETTINGS` per quality mode gives the render resolution. The feature is
created with `IsHDR | MVLowRes | DepthInverted (reversed-Z) [| AutoExposure when there is no Exposure texture]`,
recreated on size / quality / flag changes (after `waitIdle`), reset on camera cuts and released on view destruction /
shutdown. If an evaluation fails, the pass does a spatial upscale instead, so the frame is still complete.

**Input conventions.** No sign had to be changed during the bring-up.

| Input | Convention | Evidence (RTX 3080) |
| --- | --- | --- |
| Jitter | `RenderView::jitterPixels()` as is: the offset in render pixels (x right, y down) by which the jittered projection moves the image | static scene, PSNR vs 16× SSAA: 40.58 dB (TAAU 39.22 dB); any other sign gives 25–31 dB and shimmer (`DlssStaticSceneConvergesAndStaysStable`) |
| Motion vectors | the velocity buffer is `uvCurrent − uvPrevious`, unjittered, at render resolution (`MVLowRes`, no `MVJittered`); `InMVScale = (−renderW, −renderH)` | moving camera, PSNR vs 16× SSAA: 31.04 dB (TAAU 29.51 dB); with an axis flipped 28.6–28.8 dB (`DlssFollowsCameraMotion`) |
| Depth | reversed-Z, flag `DepthInverted` | — |
| Exposure | the engine's `Exposure` texture + `InPreExposure` (colour is radiance × preExposure); `AutoExposure` only without the texture | `DlssWithAutoExposureAndPreExposure`. Presets L/M (UltraPerformance, Performance) ignore the exposure inputs |
| Sharpness | `InSharpness = r.Upscaler.Sharpness` | ignored by NGX (deprecated): the cvar has no effect on DLSS |

**Quality modes** at 960×540 output (`DlssQualityModesSetRenderResolutionAndMipBias`, which also resizes the output
with the view and its NGX feature alive):

| `r.Upscaler.Quality` | Render resolution | Mip bias |
| --- | --- | --- |
| UltraPerformance | 320×180 | −2.585 |
| Performance | 480×270 | −2.000 |
| Balanced | 557×313 | −1.785 |
| Quality | 640×360 | −1.585 |
| Native (DLAA) | 960×540 | −1.000 |

## Quality presets

| CVar | Default (code) | Group: Low, Medium, High, Ultra |
| --- | --- | --- |
| `r.AntiAliasing` (core) | 0 | AntiAliasing: None, FXAA, TAA, TAA |
| `r.TAA.Quality` | 2 | AntiAliasing: 0, 1, 2, 3 (min/max clamp → variance clip → +Catmull-Rom/dilation/anti-flicker → wider) |
| `r.AntiAliasing.Samples` | 8 | AntiAliasing: 4, 8, 8, 16 (TAA jitter period) |
| `r.FXAA.Quality` | 2 | AntiAliasing: 0, 1, 2, 3 (4/8/12/16 search steps) |
| `r.Bloom` | false | PostProcess: on, on, on, on |
| `r.Bloom.Quality` | 3 | PostProcess: 1, 2, 3, 4 (4 + q mips) |
| `r.DepthOfField` | false | PostProcess: off, on, on, on |
| `r.DOF.Quality` | 2 | PostProcess: 0, 1, 2, 3 (2 + q gather rings) |
| `r.MotionBlur` | false | PostProcess: off, on, on, on |
| `r.MotionBlur.Quality` | 2 | PostProcess: 0, 1, 2, 3 (4 + 4q samples) |
| `r.Vignette` / `r.ChromaticAberration` / `r.FilmGrain` | true | PostProcess: on/off, on, on, on (CA, grain off at Low) |
| `r.TAA.Sharpness` 0.25, `r.TAA.CurrentFrameWeight` 0.08, `r.TAA.AntiFlicker` true, `r.Upscaler.Sharpness` 0.2, `r.Sharpen` 0, `r.ColorGrading` true, `r.Bloom.Intensity` 0.04 | | — |
| `r.Exposure.Auto` false, `r.Exposure.MinEV100` −4, `.MaxEV100` 20, `.SpeedUp` 3, `.SpeedDown` 1 (EV/s) | | — |

Image-changing effects are off in code so a bare `Renderer` (tests, tools) renders exactly the core image; the
runtime/editor apply a scalability level (or `applyRecommendedSettings`) which turns them on. Artistic amounts come
from volumes.

**Auto quality.** `recommendedSettings(caps, benchmarkScore, dlssAvailable)` (or `(device, score)`, which probes
NGX) = `levelsForScore` + AA/upscaler: RTX → DLSS (Ultra DLAA, High Quality, Medium Balanced, Low Performance);
otherwise High/Ultra native + TAA, Medium TAAU Quality, Low FSR 1 Balanced + TAA. `applyRecommendedSettings` sets
groups + cvars (`RuntimeRendererOptions::autoDetectQuality` uses it; the editor's Auto-Detect button should too).

## PostProcessVolumeComponent

Unbound (global) or box volumes (entity transform, half `extents`, `blendRadius` fade outside, `blendWeight`,
`priority` ascending). Per-category overrides: exposure (auto, compensation, min/max EV, speeds, histogram
percentiles), bloom (intensity, lens dirt texture + intensity), depth of field (focus distance, f-number — 0 = the
camera's `CameraComponent::aperture` —, focal length — 0 = from the FOV on a 24 mm sensor —, max bokeh size), motion
blur (shutter fraction, max length), white balance (temperature/tint, UE semantics), grading (saturation, contrast,
lift/gamma/gain), user LUT, lens (vignette, chromatic aberration, film grain, sharpen). Extracted by an extract hook
into the `PostProcessVolumesSnapshot` snapshot extension and blended per view at the camera position
(`resolvePostProcessSettings`).

## Tests, goldens, timings

CPU (`postprocess_cpu_tests.cpp`): quality modes / mip bias / jitter, availability, Auto quality, cvars + scalability,
upscaler slot & AA resolution, volume weights & blending, white balance, reflection round trip.
GPU (`gpu/postprocess_gpu_tests.cpp`, goldens `postprocess_*.png`): thin geometry no-AA / FXAA / TAA vs a 16× SSAA
reference (edge error 16.0 → 12.3 → 7.4, PSNR 30.0 → 32.8 → 38.5 dB), TAA moving object without ghosting,
FSR 1 at 50 % vs native (37.7 dB vs bilinear 35.0 dB), TAAU 50 % vs SSAA (32.1 dB vs bilinear 25.9 dB), DLSS fallback,
the DLSS tests that need an RTX GPU (skipped elsewhere; they run and pass on the RTX 3080: `DlssRendersOnRtx`,
`DlssQualityModesSetRenderResolutionAndMipBias`, `DlssStaticSceneConvergesAndStaysStable`, `DlssFollowsCameraMotion`,
`DlssWithAutoExposureAndPreExposure`, `DlssSurvivesRendererAndDeviceRecreation`), bloom on emissives, DoF near/far,
motion blur on a moving object, grading LUT +
saturation 0, inverting user LUT, lens effects, auto exposure converging from 300 lux and 100 klx to the same
brightness (plus the slower adaptation back), and `PerfReport1080p`.

GPU time on Apple M4 Pro at 1080p (ms): TAA 0.56 (+ CAS sharpen 0.18), TAAU 540p→1080p 0.55 (+0.18), FSR 1
540p→1080p EASU 0.32 + RCAS 0.15, FXAA 0.11, auto exposure 0.04, bloom (7 mips) ≈0.3, DoF ≈0.2–0.35 (setup 0.09,
gather 0.03, composite 0.19), motion blur ≈0.22 (tiles 0.06, gather 0.15), HDR composite (CA + bloom + vignette + LUT)
0.2, LDR post (grain/LUT) 0.11. MoltenVK smears timestamps of consecutive compute passes, single numbers vary ±0.1.

DLSS pass on an NVIDIA RTX 3080 at 1080p (ms): Performance 6.2, Quality 3.95, DLAA 2.9. These are rough: validation
was on and other processes shared the GPU during the measurement.

## Limits / TODO

* DLSS, still open after the RTX 3080 bring-up: the windowed player and editor were not checked by eye with DLSS
  (window resize included), the Debug build with the NGX development DLLs and their debug overlay was not run, there
  are no 4K timings, and nothing was run on Linux. The input conventions rest on the PSNR tests above.
* If NGX initialisation fails inside a `Renderer` (`DlssFeature::initialize`) on a device that was not probed before,
  that first frame runs without an upscaler; TAAU takes over once the failed probe is known. Probing first with
  `upscalerAvailability(device)` avoids it.
* DLSS frame generation / ray reconstruction are not integrated.
* DoF base values come from the view's `CameraComponent` (`focusDistance`, `focalLength`, `aperture`); volumes with
  `overrideDepthOfField` blend over them (`resolvePostProcessSettings(..., camera)`).
* TAA disocclusion uses velocity differences (no previous-depth reprojection); TAAU keeps sub-pixel wires only
  partially at 50 %. Transparent / particle velocities are not written (translucency should provide a reactive mask).
* Bloom lens dirt and user LUT textures are taken from the resource cache only once loaded (nothing while loading).
* FXAA and film grain run before the Overlay features of other areas but after the debug view bypass is decided by
  the tonemapper, so debug views also get FXAA/grain.
