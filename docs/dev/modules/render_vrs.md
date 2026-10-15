# render — variable rate shading (`VariableRateShading`)

Code: `engine/render/src/features/vrs/` (`vrs_feature.cpp` passes + cvars, `vrs_decision.cpp` CPU reference of the
rate decision, availability, pass lists, `vrs_internal.hpp` graph building blocks shared with the tests), shaders
`engine/shaders/render/vrs/`, public API `<oxwald/render/features/vrs/vrs.hpp>`. Registered by
`registerVrsFeatures(f)` in `registerBuiltinFeatures` (line `[feature-area: vrs]`). Measurements: section
[Measurements](#measurements-rtx-3080-2026-10-14) below and `docs/dev/perf.md` ("Variable rate shading"). User guide:
[chapter 27](../../guide/27-gpu-driven-performance.md) (step "Переменная частота шейдинга"), cvars in
[chapter 26](../../guide/26-quality-settings.md) (group Shading).

## What it does

`VK_KHR_fragment_shading_rate` with an **attachment rate** (a shading rate image): one R8_UINT texel per tile
(the device's attachment texel size; 16×16 px on NVIDIA Ampere) chooses the fragment size of that tile — 1×1, 1×2,
2×1, 2×2, 2×4, 4×2 or 4×4 (whatever `vkGetPhysicalDeviceFragmentShadingRatesKHR` reports for 1 sample). Coverage,
depth and stencil stay per pixel; only the fragment shader runs once per coarse fragment. The rate image is computed
per view and frame:

| Point / order | Pass | What |
| --- | --- | --- |
| AfterDepth 1000 | `VRS.Generate` (compute, 1 thread per tile) | last frame's tile statistics, reprojected with this frame's `Velocity` (tile centre), + motion (max per-axis speed of 3×3 samples) + depth-of-field CoC (min of 4×4 depth samples, thin lens like `DepthOfField`) + periphery + hysteresis + periodic refresh → `VRS.Rate` (history pair, R8_UINT, usage Storage + Sampled + ShadingRateAttachment), published as `ShadingRate` |
| AfterOpaque 1000 | `VRS.AnalyzeOpaque` (compute, one 8×8 group per tile) | statistics of the opaque `SceneColorHDR` (transient RGBA16F per tile); culled by the graph unless the frame has refraction (below) |
| BeforePostProcess −100000 | (attach) | `RenderGraph::pass(i).shadingRate(rate, {T, T})` on every declared graphics pass whose name is in `r.VRS.Passes` (default list below), that is not excluded, and whose render area equals the render extent |
| BeforePostProcess −100000 | `VRS.Analyze` (compute, one 8×8 group per tile) | statistics of this frame's `SceneColorHDR` (render resolution, before TAA / TAAU / DLSS): mean perceptual luma, RMS of horizontal and vertical luma differences → `VRS.Stats` (RGBA16F per tile) for the next frame |
| Debug | `VRS.Overlay` | `r.VRS.Mode DebugOverlay` or `r.VRS.Overlay`: tints SceneColorLDR per tile (blue 1×2/2×1, green 2×2, yellow 2×4/4×2, red 4×4; tile borders) |

### Rate decision (`oxVrsDecide` in `vrs_common.glsl` = `vrs::decideRate` in C++)

Content adaptive shading after Yang, Zhdan, Kilgariff, Lum, Zhang, Johnson, Rhu: "Visually Lossless Content and
Motion Adaptive Shading in Games" (NVIDIA, I3D 2019), per axis:

* luma `l = sqrt(Y / (1 + Y))` of the pre-exposed HDR colour (Reinhard, then ≈ display gamma);
* error of half rate ≈ RMS of the per-pixel differences `E`, of quarter rate ≈ `2.13 E`;
* just-noticeable difference `τ · (mean luma + 0.1)` (Weber with a floor), `τ = r.VRS.Threshold`;
* motion: `E / (1 + r.VRS.MotionScale · pixels moved per frame)` along that axis (the eye does not resolve detail
  that moves; with motion blur on the screen is blurred anyway);
* hysteresis (Schmitt trigger): candidates with `τ(1 − h)` (strict) and `τ(1 + h)` (loose); the new rate is the
  previous one clamped to `[strict, loose]` — coarsen only clearly below the threshold, refine at once clearly above;
* no statistics (first frame, camera cut, disocclusion: the reprojected tile is off screen, periodic refresh) → 1×1;
* depth of field: CoC ≥ 3 px → 2×2, ≥ 6 px → 4×4 (the sharpest of 16 samples decides);
* periphery: beyond `r.VRS.Periphery` (normalised distance, 1 = corner) 2×2, beyond the middle of the rest 4×4;
* clamp to `r.VRS.MaxRate`, then the largest supported size ≤ the result (the larger axis shrinks first:
  1×4 → 1×2 on NVIDIA).

`τ` is much smaller than the paper's examples suggest: with this error model a 2×2 fragment on a ramp of `E` per pixel
leaves steps of about `2E`, so τ = 0.08 (the first default) let smooth lighting and fine texture detail go visibly
blocky / blurred. The presets use 0.02–0.05 (measurements below).

### Feedback loop and its traps (fixed 2026-10-14)

The statistics are measured on an image that was itself shaded at the chosen rates. Three ways this went wrong, all
found by the golden tests and the showcase measurements:

1. **Coarse tiles under-read their own detail.** Inside a coarse fragment all pixels are equal, so `VRS.Analyze`
   reads this frame's rate and differentiates between neighbouring coarse fragments (stride `s`). Dividing that
   difference by `s` (a per-pixel gradient) read edges and texture detail `s` times too low, so a tile that once went
   coarse stayed coarse. Now the difference is divided by `√s`: a step edge reads exactly as at full rate, a linear
   ramp reads `√s` too high (conservative, and inside the hysteresis band, so it does not oscillate).
2. **Texture LOD lock-in.** Derivatives scale with the fragment size, so a 2×2 tile samples its textures one mip
   coarser and the measured detail drops further; detail that appears later (texture streaming finishing, lights,
   animation) was never seen. `r.VRS.RefreshPeriod` (default 8): every N frames each tile (staggered pattern
   `(x + 3y + frame) mod N`) is treated as having no statistics, i.e. shaded at 1×1 once and measured properly; the
   hysteresis keeps it coarse only if it really is smooth. Wooden floor of showcase station 02 at 1080p, τ = 0.02:
   PSNR 39.3 → 56.4 dB, FLIP 0.0185 → 0.0044, the wood grain is back.
3. **Refraction.** The opaque passes of a tile are shaded at the rate the *final* image of that tile asked for.
   Behind refractive glass or water the final image shows the opaque surface displaced and magnified, so a detailed
   floor behind a smooth-looking sphere went coarse and the refraction showed it (TranslucencyTest's sphere: mean
   error 2.8 / 255, worst 16×16 block 27). `VRS.AnalyzeOpaque` measures the opaque image at AfterOpaque and
   `VRS.Analyze` keeps per axis the larger error relative to the JND of the two. Only when the graph has
   `Translucency.Refractive` or `Water.Surface`; otherwise the transient output is unused and the graph culls the
   pass (fog and particles only hide detail, so the final image is the right measure there).

Detail that is completely invisible at the coarse resolution is still only found by the refresh (inherent to the
method).

## Where the rate image goes (and where not)

Default `r.VRS.Passes`: `ForwardOpaque, World.Forward, Sky, WorldSky, Water.Caustics, Volumetrics.Composite,
Translucency.Refractive, Translucency.OIT, Translucency.Sorted, Water.Surface, Particles.Render`. A pass gets it only
if it is a graphics pass with an automatic rendering scope at the render extent: `Particles.Render` at
`r.Particles.ResolutionDivisor` 2/4 renders at a lower resolution and keeps full rate (the tiles would not line up).
The list is parsed only when `r.VRS.Passes` changes (`vrs::parsePassList`); attaching allocates nothing.

Never (`vrs::isExcludedPass`, even if listed): `DepthPrepass*`, `World.Prepass` — they write `Normals`, `Velocity`,
`EntityID`, which TAA / TAAU / DLSS (motion vectors, depth), SSR / SSAO and picking read per pixel, and coarse
shading there would also break hashed alpha (alpha-tested coverage is decided per pixel in the prepass, the colour
passes test depth EQUAL); `Shadow*`, `World.Shadow*` (depth only, different resolution); `HiZ`, depth copies,
`Particles.LowResDepth` / `.Composite`; TAA / TAAU / DLSS / FSR / Resample / Tonemap / Final / post-processing
(resolution-exact filters); UI, editor overlays, debug views, picking; reflection captures, IBL and ray tracing
passes. DLSS and TAA still receive full-resolution, per-pixel depth and motion vectors; only the colour they
accumulate is shaded more coarsely in flat or moving regions.

## RHI support (shared files, additive)

* `DeviceCaps`: `pipelineShadingRate`, `attachmentShadingRate`, `primitiveShadingRate`, attachment texel size range
  and aspect, `shadingRateMask` (supported fragment sizes, bit = attachment encoding), non-trivial combiner ops,
  depth/stencil-write support; `shadingRateSupported(w, h)`; printed by `toString()`.
* `DeviceDesc::enableFragmentShadingRate` (default true; false = extension not enabled → VRS is a no-op).
* `Access::ShadingRateRead` (stage FRAGMENT_SHADING_RATE_ATTACHMENT, layout FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL),
  `TextureUsage::ShadingRateAttachment`.
* `PassBuilder::shadingRate(rate, texelSize)`, `RenderGraph::pass(i)` (builder of a declared pass),
  `passType(i)`, `passRenderExtent(i)`, `passShadingRate(i)`; `RenderingDesc::shadingRate` /
  `shadingRateTexelSize` (`VkRenderingFragmentShadingRateAttachmentInfoKHR`).
* Graphics pipelines (`GraphicsPipelineDesc::variableRateShading`, default true) are created with
  `VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR` and combiner ops (KEEP, REPLACE) on devices
  with attachment rates. Required by Vulkan for any draw inside a rendering scope that has a rate image; without an
  attachment the rate is 1×1. Verified bit-exact: `RenderTest.VrsOffMatchesDeviceWithoutTheExtension` renders
  implicit-derivative texturing through refraction with r.VRS off on a device with and without the extension —
  identical bytes (an early suspicion that the flag alone changed the golden images was wrong: the tests ran with
  VRS on through a leaked Shading = High preset, see below).

## CVars

| CVar | Default | Shading: Low / Medium / High / Ultra |
| --- | --- | --- |
| `r.VRS` | off | **on / on / off / off** |
| `r.VRS.Mode` | Adaptive (Adaptive, Motion, Foveated, DebugOverlay) | — |
| `r.VRS.Threshold` | 0.02 | 0.05 / 0.03 / 0.02 / 0.02 |
| `r.VRS.MaxRate` | 2 | 4 / 2 / 2 / 2 |
| `r.VRS.Hysteresis` | 0.25 | — |
| `r.VRS.RefreshPeriod` | 8 (0 = off) | — |
| `r.VRS.MotionScale` / `.MotionPixels` | 0.15 / 6 | — |
| `r.VRS.Periphery` | 0 (off; Foveated mode uses 0.5) | — |
| `r.VRS.DepthOfField` | on | — |
| `r.VRS.Overlay` | off | — |
| `r.VRS.Passes` | empty = default list | — |

Why: the code default is off like SSR / GTAO / bloom, so a bare `Renderer` (tests, tools) renders exact images — the
first default (on, τ 0.08) changed every golden image of the render tests (5 golden failures + 3 GPU-driven image
comparisons). On the RTX 3080 the gain at High (τ 0.02, visually lossless) was 0–6 % of the GPU frame, not worth
any risk at the levels that promise full quality; Medium (τ 0.03, PSNR ≥ 44.7 dB, FLIP ≤ 0.016 on every station) and
Low (τ 0.05, 4×4, slight texture blur, PSNR ≥ 39 dB) trade a little detail for the shading cost on the GPUs those
levels are for. The High / Ultra threshold values are what a player gets when switching r.VRS on by hand.

## Cost of the analysis

`VRS.Analyze` reads the colour target once per pixel (+ the neighbour of each pixel, from the texture cache) and
reduces per tile with `subgroupAdd`: 0.10 ms at 4K, ≈0.03 ms at 1080p (RTX 3080); `VRS.AnalyzeOpaque` the same when
present; `VRS.Generate` 0.025 ms at 4K. An earlier version with a 64-lane shared-memory tree reduction cost
0.42 ms per analysis at 4K, which ate most of the gain; a shared-memory luma cache (with integer division in the
fill loop) was slower than relying on the texture cache. The analysis needs `GL_KHR_shader_subgroup_arithmetic`
(every device with attachment shading rates has it); without attachment rates the feature compiles no pipelines.

## Measurements (RTX 3080, 2026-10-14)

`ShowcaseVrs.StationsQualityAndPerformance`, RelWithDebInfo, validation off, under the GPU lock (other agents'
functional GPU tests may still have run), showcase stations in photo mode at quality High with the VRS cvars
overridden, fixed 1/60 s step, medians of frames 121–180, quality of frame 181 against the r.VRS-off frame. TAA on.
Cells: GPU frame change / change of the passes that get the rate image (ms) · PSNR dB · FLIP. "Off again" (run-to-run
noise): frame ±1–2 %, PSNR ≥ 66 dB, FLIP ≤ 0.0015 — except one outlier: the 06_World 1080p Off frame read 2.85 ms
against 2.61 ms in the repeat, so its frame percentages are ~9 points too good (use the pass column there).

| station | size | Off: frame / affected ms | VRS passes ms | Low (τ 0.05, 4×4) | Medium (τ 0.03, 2×2) | τ 0.02, 2×2 (High if switched on) |
|---|---|---|---|---|---|---|
| 01_Lighting | 1080p | 2.75 / 0.68 | 0.04 | −6 % / −0.21 · 41.1 · 0.016 | −5 % / −0.17 · 46.4 · 0.009 | −3 % / −0.13 · 46.9 · 0.009 |
| 01_Lighting | 4K | 7.63 / 1.98 | 0.13 | −8 % / −0.86 · 43.4 · 0.013 | −8 % / −0.68 · 49.3 · 0.007 | −7 % / −0.62 · 49.8 · 0.007 |
| 02_Materials | 1080p | 2.56 / 0.21 | 0.07 | −0 % / −0.05 · 39.9 · 0.021 | +1 % / −0.02 · 48.9 · 0.008 | +2 % / −0.01 · 56.4 · 0.004 |
| 02_Materials | 4K | 6.68 / 0.70 | 0.24 | −2 % / −0.28 · 40.8 · 0.022 | +0 % / −0.17 · 48.1 · 0.010 | +1 % / −0.11 · 53.1 · 0.007 |
| 04_Volumetrics | 1080p | 3.57 / 0.37 | 0.04 | −4 % / −0.18 · 43.9 · 0.016 | −3 % / −0.15 · 49.9 · 0.009 | −3 % / −0.14 · 52.0 · 0.008 |
| 04_Volumetrics | 4K | 8.61 / 1.26 | 0.14 | −8 % / −0.74 · 46.4 · 0.013 | −7 % / −0.62 · 51.8 · 0.008 | +2 %* / −0.61 · 53.6 · 0.007 |
| 05_Water | 1080p | 2.85 / 0.68 | 0.07 | −5 % / −0.20 · 44.6 · 0.014 | −2 % / −0.14 · 50.1 · 0.009 | −0 % / −0.09 · 54.8 · 0.006 |
| 05_Water | 4K | 8.37 / 2.36 | 0.24 | −6 % / −1.05 · 44.9 · 0.014 | −9 % / −0.80 · 50.1 · 0.008 | −6 % / −0.62 · 53.4 · 0.007 |
| 06_World | 1080p | 2.85 / 0.60 | 0.04 | −13 %* / −0.18 · 39.8 · 0.025 | −10 %* / −0.10 · 46.7 · 0.011 | −9 %* / −0.06 · 51.9 · 0.007 |
| 06_World | 4K | 7.44 / 1.94 | 0.13 | −12 % / −0.89 · 39.1 · 0.031 | −6 % / −0.60 · 44.6 · 0.016 | −4 % / −0.42 · 48.2 · 0.011 |

\* noise (see above). VRS passes: 0.04 ms at 1080p / 0.13 ms at 4K (`Generate` 0.01 / 0.03 + `Analyze` 0.03 / 0.10),
twice the analysis with refraction (02 glass, 05 water): 0.07 / 0.24 ms — there the gain at τ ≤ 0.03 is eaten.

Other modes (same run, full table in `docs/dev/perf.md`): **Motion** does nothing on a static camera (+0.04 ms cost)
and with the camera panning 0.6°/frame saves as much as Adaptive τ 0.03 (01: −5 % 1080p / −7 % 4K, 05 4K −11 %;
PSNR 39–57 dB against the panning Off run). **Foveated** (periphery 0.5, 4×4): −2…−13 % (the 02 1080p run hit a 5 ms hitch and is not counted), but PSNR 33–46 dB, FLIP
up to 0.030 — the corners visibly blur on a flat monitor; meant for HMDs / eye tracking. The first defaults
(τ 0.08, 2×2 at High; 0.12 / 0.16 with 4×4 at Medium / Low) were barely faster than τ 0.05 and lost texture
detail visibly (02 1080p PSNR 37.8 dB, FLIP 0.025).

Golden scenes (`ScalabilityPresetsKeepGoldenScenesClean`, against r.VRS off; mean error / worst 16×16 block in 1/255,
FLIP):

| scene | Low | Medium | High / Ultra (if switched on) | old High (τ 0.08 before the fixes) |
|---|---|---|---|---|
| many point lights 256² | 1.23 / 4.45 / 0.0073 | 0.13 / 1.65 / 0.0012 | 0.03 / 1.26 / 0.0004 | 2.44 / 6.42 / 0.0127 |
| many point lights 1280×720 | 1.27 / 4.79 / 0.0136 | 0.68 / 2.87 / 0.0050 | 0.41 / 2.18 / 0.0042 | 1.12 / 6.76 / 0.0063 |
| refractive sphere 256² | 0.00 / 0.00 / 0 | 0.00 / 0.00 / 0 | 0.00 / 0.00 / 0 | 2.13 / 26.9 / 0.0374 |
| refractive sphere 1280×720 | 0.06 / 3.03 / 0.0008 | 0.01 / 1.30 / 0.0002 | 0.01 / 0.77 / 0.0001 | 0.65 / 23.0 / 0.0121 |

Visual check (2× crops): station 01 at τ 0.02 indistinguishable from Off; 02 wooden floor at τ 0.02 lost its grain
before the refresh fix and keeps it after; 06 grass at 4K: Medium indistinguishable, Low (4×4) visibly softer.

## Tests

`tests/vrs_cpu_tests.cpp` (CPU, 9): flat → 4×4, edges → 1×1, stripes → full rate across only, max rate and supported
fallback, hysteresis band (an error oscillating around the threshold flips the rate every frame without hysteresis,
never with it), motion lowering the error, Motion / Foveated / depth-of-field modes, availability reasons, exclusion
list, `parsePassList`, image metrics (PSNR / SSIM / FLIP).

`tests/gpu/vrs_tests.cpp` (`render;gpu`, `GTEST_SKIP` without attachment rates):
`RateImageFromSyntheticInputs` (Analyze + Generate on a flat / stripes / checker image),
`GenerateShaderMatchesCpuReference` (random statistics and previous rates; Motion mode with uniform motion),
`PassExclusionListRespected` (only listed passes, excluded ones even when listed), `ImageWithinPsnrOfVrsOff`
(PSNR / SSIM / FLIP bounds, coarse tiles > 20 %, debug overlay visible), `TemporalStabilityWithHysteresis` (TAA
jitter, tiles changing per frame, refresh off), `ScalabilityPresetsKeepGoldenScenesClean` (the scenes of
`GoldenTest.ManyPointLightsClustered` and `TranslucencyTest.RefractiveSphereAbsorption` at 256² / 4 frames and
1280×720 / 8 frames with the threshold / max rate of every level against r.VRS off: mean error, worst 16×16 block,
FLIP bounds per level; r.VRS only in Low / Medium, off by default; `VRS.AnalyzeOpaque` only with refraction;
`OX_VRS_SWEEP=0.02,0.03,...` prints a threshold sweep), `RuntimeToggleWithoutLeaks`,
`RenderTest.VrsNoOpWithoutTheExtension` (runs everywhere: device without the extension, image identical with r.VRS
on and off) and `RenderTest.VrsOffMatchesDeviceWithoutTheExtension` (r.VRS off: same bytes with and without the
extension). Opt-in `RenderTest.VrsPipelineStateOverhead` (`OX_VRS_PERF=1`). Validation errors fail every test.

Measurements: `ShowcaseVrs.StationsQualityAndPerformance` in `ox_showcase_tests` (opt-in: `OX_VRS_PERF=1
OX_VULKAN_VALIDATION=0`; `OX_VRS_PERF=01_Lighting,05_Water` only these stations, `OX_VRS_RES=3840x2160` one size,
`OX_VRS_CONFIGS=Adaptive_t002_max2,...` only these configurations, `OX_VRS_OUT=E:/...` output directory — the PNGs
are ~1 GB with 4K). CSV columns: frame / affected / VRS-pass medians, coarse tiles, PSNR / SSIM / FLIP against the
Off run, per-pass `VRS.Generate` / `.Analyze` / `.AnalyzeOpaque`. The HUD's FPS counter (bottom right) is masked out
of the comparison. `vrs_image_metrics.hpp` has the metrics.

## Platforms

* NVIDIA RTX 3080 (driver 617.42): attachment texel 16×16, rates 1×1 … 4×4 (no 1×4 / 4×1). Verified there.
* MoltenVK: no `VK_KHR_fragment_shading_rate` → feature disabled, no VRS pipelines (Metal has rasterization rate
  maps, which this path does not use). Not run on the Mac in this session.
* Android: devices with the extension use it (texel sizes 8–32 are handled); untested.

## Known limits / TODO

* The test fixtures that call `scalability::setGroup(...)` (e.g. `TranslucencyTest` sets Shading = High) do not
  restore the levels, so later tests in the same process run with that preset. Harmless for VRS now (High has it
  off), but it is how VRS leaked into unrelated golden tests while High had it on.
* One rate image per view at render resolution; no per-draw (primitive) rates (`primitiveShadingRate` unused), no
  MSAA interaction (the renderer has no MSAA path).
* Statistics are one frame late and reprojected per tile (centre velocity): fast disocclusions shade at 1×1 for a
  frame, which is the safe direction.
* Textures in coarse tiles are sampled one mip blurrier (derivatives); a shader-side LOD bias from
  `gl_ShadingRateEXT` would keep them sharp but needs the extension in every material shader.
* Pass selection is by name: a renamed pass silently loses VRS (the tests list the passes that got it).
* Gains on the RTX 3080 are small because the forward passes are cheap there (0.2–2.3 ms at 4K on the showcase
  stations); heavier materials and weaker GPUs gain more. Not measured on a Low / Medium class GPU.
