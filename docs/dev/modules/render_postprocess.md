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

### DLSS Ray Reconstruction (`upscalers/dlss_rr_feature.cpp`, `dlss_ngx.cpp`, `raytracing/rt_rr_guides.cpp`)

`r.Denoiser` (Persist, no scalability levels): `SVGF` (0, code default), `DLSSRR` (1, aliases `RR`,
`RayReconstruction`), `Auto` (2: Ray Reconstruction where available, else SVGF), `Neural` (3, see
[render_neural.md](render_neural.md)). The Auto quality recommendation, the runtime `UserSettings.graphics.denoiser`
default and the editor / in-game settings menus use `Auto`. `rayReconstructionSelected(settings, caps)` is true when
the cvar asks for it, ray tracing is active, the path tracer is off and NGX has not reported Ray Reconstruction
unavailable; `rayReconstructionAvailability(device)` gives the reason otherwise (same texts as DLSS plus
`SuperSamplingDenoising.Available / NeedsUpdatedDriver / FeatureInitResult`, probed together with DLSS).

* **Feature `DLSSRR`**: exclusive group `Upscaler`, priority 400 (above DLSS 300, TAAU 200, FSR 1 100), so it replaces
  DLSS super resolution / TAAU / TAA; `effectiveUpscaler` reports DLSS while it is selected. Render resolution:
  `r.Upscaler.Quality` through `DLSSDOptimalSettingsCallback` (same sizes as DLSS SR, see the table above; tested by
  `QualityModesResizeAndRecreate`), DLAA when `r.Upscaler` is Off. `prepareView` marks the view
  (`rayReconstructionRunsThisFrame(view)`), and the ray traced shadows / AO / reflections then skip their SVGF
  temporal + variance + à-trous passes for that frame (`rt::svgfActive(ctx)`); only the format resolve / half-res
  upsample stays. The rays themselves are unchanged (1 spp).
* **Inputs** (`NGX_VULKAN_CREATE_DLSSD_EXT1` / `NGX_VULKAN_EVALUATE_DLSSD_EXT`, flags `IsHDR | MVLowRes |
  DepthInverted`, `AutoExposure` only without the exposure texture): the noisy `SceneColorHDR` at `BeforePostProcess`,
  hardware depth (`NVSDK_NGX_DLSS_Depth_Type_HW`), velocity / jitter / exposure / pre-exposure with the DLSS SR
  conventions, `Normals` (world normal + perceptual roughness in w, `NVSDK_NGX_DLSS_Roughness_Mode_Packed`),
  `RR.DiffuseAlbedo` = baseColor × (1 − metallic) and `RR.SpecularAlbedo` = F0 × A + B (Karis' analytic split-sum
  term for the G-buffer normal / roughness) from `RayReconstructionGuidesRT` (`rr_guides.comp`: the renderer is
  forward+, so a short primary ray query in a window around the prepass depth finds the material again; ~0.07 ms at
  720p, 0.27 ms at 1440p), `RR.SpecularHitDistance` written by `reflections.comp` (flag bit 2, world units, misses
  1e4), world → view and unjittered view → clip matrices. Missing guides fall back to white / black defaults.
* **Preset**: E (latest transformer model) for every quality mode, set through the
  `RayReconstruction.Hint.Render.Preset.*` parameters before creation. Against the default (F) on the quality test
  scene: +0.2 dB against the path tracer, +0.1–0.3 dB against the converged hybrid frame, DLAA flicker 0.045 vs 0.088.
  The price: the RR pass is 4–15 % slower than with F (1080p Quality 4.28 vs 4.10 ms, DLAA 9.12 vs 8.16 ms; 4K
  Quality 16.5 vs 14.6 ms, DLAA 36.2 vs 31.2 ms).
* **Lifetime**: one NGX feature per view, recreated (after `waitIdle`) when size / quality / flags change, released
  with the view and at shutdown. The companion feature `DLSSRRRelease` (enabled while a view still holds a feature
  and RR is no longer selected) gives the feature back as soon as RR is switched off at run time.
  `ngxLiveFeatureCount()` counts live NGX features (DLSS SR + RR) for diagnostics and tests. The DLSS SR feature of a
  view stays allocated while RR runs (two features, `RuntimeToggleWithoutLeaks`). NGX keeps the RR network and its
  device-wide working memory after `ReleaseFeature` until NGX shuts down with the device: driver-reported VRAM rises
  by ~180–510 MB on the first RR frame (it varied between runs) and does not come back when RR is switched off;
  destroying the whole `Renderer` returns only ~120 MB of it. Repeated toggling does not grow it (6 toggles: ±0 MB,
  same texture / buffer counts).
* **NGX failure** in a frame: an EASU spatial upscale of the noisy frame keeps the output complete (logged once).

**Image quality** (`ImageQualityAndStabilityVersusSvgfPlusDlss`, 640×360 output, rough gold sphere, glossy metal slab,
sun + two shadowed point lights; PSNR of 2×2-downsampled images; RTX 3080, driver 617.42):

| Configuration | vs path tracer (4096 spp) | vs converged hybrid | Flicker (mean \|ΔL\|, 0..255) |
| --- | --- | --- | --- |
| SVGF + DLSS Quality | 25.81 dB | 34.54 dB | 0.037 |
| RR Quality | 25.32 dB | 37.82 dB | 0.089 |
| SVGF + DLAA | 25.67 dB | 37.12 dB | 0.031 |
| RR DLAA | 25.32 dB | 38.19 dB | 0.045 |

"Converged hybrid" is the same lighting model without a denoiser (32 rays per pixel for shadows / reflections / AO,
TAA over 64 frames): against it RR is the better denoiser by 3.3 dB (Quality) and 1.1 dB (DLAA), and it removes the
SVGF sparkle on the rough gold. Against the path tracer it is 0.35–0.5 dB lower: both denoisers see hybrid lighting
that misses the second bounce inside reflections (the spheres' undersides in the slab are ~0.15 too dark in luminance
for both), and SVGF's residual sparkle in those reflections adds some of the missing energy back by chance. Ruled out
as RR input errors (each measured): the specular albedo guide, roughness as perceptual vs squared (Unpacked mode with
α: −0.01 dB), the hit distance (no change without it), the camera matrices (without them RR flickers 3× more), the
reflection firefly clamp (16× vs 64× display white: identical). The test asserts RR ≥ SVGF − 0.25 dB against the
converged hybrid and ≥ SVGF − 0.6 dB against the path tracer. Flicker stays within max(1.25 × SVGF, 0.3).

**GPU time**, RTX 3080, validation off, `PerfReportRayReconstruction` (the 240-object scene of
`RayTracingTest.PerfReportRayTracing`, 8 frames after 48 warm-up frames, ms):

| Output | Mode | SVGF + DLSS frame | RR frame | RT passes SVGF / RR | DLSS pass / RR pass |
| --- | --- | --- | --- | --- | --- |
| 1920×1080 | Quality (1280×720) | 5.95 | 7.18 | 4.53 / 2.20 | 0.74 / 4.28 |
| 1920×1080 | Performance (960×540) | 5.06 | 4.70 | 3.24 / 1.77 | 1.30 / 2.43 |
| 1920×1080 | DLAA | 9.97 | 13.74 | 8.11 / 3.49 | 0.74 / 9.12 |
| 3840×2160 | Quality (2560×1440) | 17.53 | 24.47 | 13.47 / 6.01 | 2.17 / 16.51 |
| 3840×2160 | Performance (1920×1080) | 14.34 | 13.52 | 8.32 / 3.24 | 4.78 / 9.05 |
| 3840×2160 | DLAA | 35.17 | 49.02 | 29.32 / 9.21 | 2.25 / 36.16 |

RR saves the SVGF stages (2.5 ms at 720p, 9.2 ms at 1440p render resolution) but its network costs more than they do
on an RTX 3080 except in Performance mode; it is a quality option there, not a speed-up. Native SVGF without AA:
9.28 ms at 1080p, 33.37 ms at 4K.

## Frame generation (FSR 3, `features/framegen/frame_generation.cpp`)

`r.FrameGeneration` (Persist): `Off`, `FSR3` (alias `On`). AMD FidelityFX FSR 3.1 frame interpolation through ffx-api
(`amd_fidelityfx_vk.dll` of FidelityFX SDK v1.1.4, the last release with a Vulkan backend; vcpkg overlay port
`amd-fidelityfx-vk`, loaded with `LoadLibrary` at run time, CMake option `OX_ENABLE_FSR3_FRAMEGEN`, a stub without the
package or off Windows). Runtime player only (`RuntimeRenderer`): the editor viewport never presents through the
proxy. Vendor independent (verified on the RTX 3080).

* **Device**: `framegen::appendVulkanRequirements(desc)` before `rhi::Device::create` asks for three private queues
  (`DeviceDesc::frameGenerationQueues`: present + image acquire in the graphics family, one compute) and
  `VK_KHR_get_memory_requirements2` / `VK_KHR_dedicated_allocation` (the FidelityFX backend loads the KHR aliases).
* **Swapchain proxy**: `rhi::Swapchain::setProxy(ISwapchainProxy*)`; the next `acquire()` tears the plain swapchain
  down and lets the generator create FSR's replacement swapchain (`ffxCreateContext(FGSWAPCHAIN_VK)`) plus the frame
  generation context; acquire / present / images go through the replacement functions. Toggling off leaves the
  proxy entirely (contexts destroyed, plain swapchain recreated) instead of paying its copy per present. A failed
  creation is not retried every frame (only after toggling off and on).
* **Reflex and frame generation**: NVIDIA Reflex cannot drive the proxy. The `VkSwapchainKHR` FSR 1.1.4 returns is
  its own object (`FrameInterpolationSwapChainVK`), not a driver swapchain, so `vkSetLatencySleepModeNV` /
  `vkLatencySleepNV` / `vkSetLatencyMarkerNV` / `vkGetLatencyTimingsNV` on it are invalid; the real swapchain is
  private to the DLL (no query for it, no latency hooks in ffx-api for Vulkan), and the proxy presents both frames from
  its own present thread on its own queue without `VkPresentIdKHR`, so the driver could not tie presents to our frame
  ids anyway. While the proxy owns the swapchain `Swapchain::lowLatencySupported()` is false (`lowLatencyCapable()`
  stays true; the renderer reports `LatencyStats::reflexBehindFrameGeneration`, the player prints
  `reflex=framegen-proxy`, the settings menu explains it), and `r.Reflex On` runs the engine limiter
  (`r.Reflex.Fallback`, default JustInTime) — the latency reducer AMD recommends next to FSR 3. Leaving the proxy
  brings Reflex back on the plain swapchain (tested). Numbers: `docs/dev/perf.md` "Latency".
* **Per frame** (render thread): `beginFrame` (proxy on/off) → `acquire` → `prepareFrame` (configure: frame id,
  HUD-less image, our present callback) → the main view records, at the Overlay injection point (order −50000, after
  LdrPost, before editor overlays and UI), a copy of `SceneColorLDR` into the HUD-less image and the
  `FrameGeneration.Prepare` dispatch (render-resolution depth, reversed Z, velocity scaled to current → previous in
  pixels, jitter, camera) → `present` (FSR interpolates on the game queue inside present; `allowAsyncWorkloads =
  false`, so the HUD-less image needs no double buffer) → `endFrame` (stats).
* **Composition is always ours** (`presentCallback`, FSR's present thread): with an sRGB back buffer FSR writes the
  interpolated image as *linear* values into its sRGB-typed bytes (measured: generated bytes = sRGB-decode of the
  expected bytes on every pixel), so a raw copy shows every generated frame far too dark. Generated frames go copy →
  UNORM scratch image → `vkCmdBlitImage` into the sRGB swapchain image (the blit encodes); rendered frames and UNORM
  back buffers are copied unchanged. Typing the back buffer / HUD-less image UNORM for FSR instead does not work
  (context creation fails on a HUD-less / back buffer format mismatch, and with both UNORM the output barely
  interpolates: 52 dB vs the *current* frame).
* **Stats** (`framegen::Stats`, `RenderStats::frameGeneration`, `OxwaldPlayer --stats`): active, rendered and
  presented frames since activation (ratio 2.0), composed generated / rendered frames (`generatedPresented`,
  `realPresented`: the proxy's pacer drops a whole scheduled pair when its present thread falls behind —
  `composeAndPresent_presenterThread` overwrites a pending entry — so the total ratio dips below 2 on a loaded machine
  while generated : rendered stays 1 : 1), `addedLatencyMs` — how long a rendered frame is held by the proxy
  between the engine's present call and the proxy composing it for its own present (after the generated frame in
  front of it; CPU timestamps, the pacing wait after composition is not included).
* **Hangs fixed during bring-up**: `capturedFrame()` waited in `WAIT_FOR_PRESENTS` while the proxy still expected the
  next frame (now: the capture copy sets a `VkEvent`, polled with a 2 s bound); `rhi::Device::waitIdle` used
  `vkDeviceWaitIdle`, which also waits for the proxy's private queues (used by its threads concurrently, and possibly
  holding work that waits for a later present) — leaving the proxy hung there; it now waits for the engine's own
  queues under their submit mutexes. The GPU tests run under a watchdog that turns a hang into a failure naming the
  phase (60 s without progress).

Tests (`gpu/frame_generation_tests.cpp`, real 640×360 window, skipped without a display or the DLL):
`PresentsOneGeneratedFramePerRenderedFrame` (150 frames: generated : rendered composed 1 ± 0.03, total ratio
> 1.6 and ≤ 2.02; idle machine 1.96–1.99, 1.80–1.89 seen next to other jobs from dropped pairs — the earlier
`2.0 ± 0.1` bound failed 3 of 12 runs that way), `GeneratedFrameMatchesTheRenderedIntermediateFrame`
(panning camera, captured generated frame vs a reference rendered at the intermediate camera: 40.95 dB, vs the
neighbouring rendered frames 35.2 / 35.0 dB; images `<temp>/oxwald_render_out/framegen_*.png`),
`TogglingInAndOutOfTheProxyIsClean` (3 rounds, no validation errors or leaked objects at device destruction; Reflex
hidden but capable through the proxy, `latencySleep` a no-op there, back on the plain swapchain).

**Proxy stalls (FidelityFX SDK 1.1.4) and what we do about them.** Symptom in both cases: FSR's present thread inside
the driver's `vkQueuePresentKHR` (`NtDxgkSubmitPresentToHwQueue`), its interpolation thread and our render thread (in
the proxy's `vkQueuePresentKHR`) waiting on the proxy's semaphores forever, while our own GPU work is complete
(graphics timeline = last submitted; the test watchdog prints both). No engine lock is involved.
* *Dropped pairs, one process.* The proxy's interpolation thread overwrites a scheduled pair (generated + rendered)
  that its present thread has not taken yet; after such drops the proxy stalled. GPU bound in the player (Showcase
  station 4 at 1440p Ultra, also station 1 at 1440p High and station 4 at 720p) that was 11 of 12 mailbox runs within
  seconds; in the 640×360 test about 1 in 50. Fix: `Fsr3FrameGenerator::present` hands frame N over only once the
  present thread composed a frame of N − 1 (present callback frame id; condition variable, 250 ms bound, so a present
  thread that composes nothing — minimised window — only costs the timeout; `Stats::pacingWaitMs`,
  `Stats::pacingTimeouts`). After the fix: 0 of 12 player runs, 0 of 30+ test runs. FIFO and immediate present modes
  without the fix: 1 of 1 and 1 of 2 runs survived — not a fix.
* *Several proxies at once.* Two or three processes each presenting through an FSR 3 proxy on the same GPU stall within
  a few frames or in `ffxDestroyContext` (9 of 9 runs with three concurrent test processes, which `ctest -j` produces
  since every gtest case is its own process), with or without the fix above; next to other GPU suites or other windowed
  (non-proxy) presenters 0 of 11. Not fixable on our side; the GPU tests serialise themselves with a named Win32 mutex
  (`Local\OxwaldFrameGenerationGpuTest`). Two games with FSR 3 frame generation running at once on one GPU are
  affected the same way.
Known artefact: FSR leaves a few 1-px pixels of the left border column un-interpolated (raw back buffer values among
linear ones, so they show darker after our encode). Timings (`docs/dev/perf.md` "Latency"; RTX 3080, Showcase
station 4, 2560×1421 window, Ultra, vsync off, medians of 3 runs): without frame generation 104 fps; with it 81.5
rendered / 163 presented fps (our GPU frame +0.7 ms for the HUD-less copy and the prepare dispatch; interpolation and
composition run outside our timestamps),
rendered frames held 24 ms by the proxy; with the engine limiter (`r.Reflex On` → JustInTime) 74 / 147 fps and engine
latency 46 → 21 ms; Strict 80 / 159 fps, 12.5 ms, hold 13 ms. Station 1 with vsync at 100 Hz: 50 rendered / 99.9
presented.

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

Ray Reconstruction: CPU `postprocess_rr_cpu_tests.cpp` (`RayReconstructionCpu.*`, 5: cvar names / default, selection
gating, upscaler slot and quality, Auto quality, availability reason without a device); GPU
`gpu/postprocess_rr_tests.cpp` (`RayReconstructionTest.*`, need an RTX GPU with `nvngx_dlssd`, images
`<temp>/oxwald_render_out/rr_*.png`): `CreatesAndEvaluatesWithoutSvgfPasses`, `QualityModesResizeAndRecreate`,
`ImageQualityAndStabilityVersusSvgfPlusDlss`, `RuntimeToggleWithoutLeaks`, `FallsBackToSvgfWhereUnavailable` (on the
RTX machine it recreates the device without the NGX extensions) and `PerfReportRayReconstruction` (`OX_RT_PERF=1`
for 1080p + 4K, or `OX_RT_PERF=WxH`).

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
* DLSS frame generation is not integrated (FSR 3 frame generation is, above). Ray Reconstruction (above) is not checked in the windowed player / editor
  by eye, not on Linux, and not on other RTX generations (the RR network is expensive on the RTX 3080).
* DoF base values come from the view's `CameraComponent` (`focusDistance`, `focalLength`, `aperture`); volumes with
  `overrideDepthOfField` blend over them (`resolvePostProcessSettings(..., camera)`).
* TAA disocclusion uses velocity differences (no previous-depth reprojection); TAAU keeps sub-pixel wires only
  partially at 50 %. Transparent / particle velocities are not written (translucency should provide a reactive mask).
* Bloom lens dirt and user LUT textures are taken from the resource cache only once loaded (nothing while loading).
* FXAA and film grain run before the Overlay features of other areas but after the debug view bypass is decided by
  the tonemapper, so debug views also get FXAA/grain.
