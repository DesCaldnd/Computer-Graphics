# Performance log

Rule from the roadmap: every optimisation starts with a measurement and ends with one. GPU times come from the render
graph's per-pass timestamps (`RenderStats::passes`, `gpuFrameMs` = sum of top-level passes), CPU times from
`RenderStats::cpuRenderMs` / wall clock around `beginFrame … endFrame` (Tracy zones `OX_PROFILE_ZONE` exist on every
step for deeper captures). Machine: Apple M4 Pro, MoltenVK 1.4.2, RelWithDebInfo, validation on (numbers in Release
are lower on the CPU side). All tables are reproducible with the named tests (in `ox_render_gpu_tests` unless the
section names another binary, e.g. `ox_showcase_tests` or `OxwaldPlayer`). The numbers measured on the Windows
machine are in their own sections, [NVIDIA RTX 3080](#nvidia-rtx-3080-2026-10),
[Neural rendering](#neural-rendering-rtx-3080-2026-10-14),
[Frame pacing](#frame-pacing-editor-viewport-and-windowed-player-rtx-3080-2026-10-14) and
[Latency](#latency-nvidia-reflex-engine-limiter-fsr-3-frame-generation-rtx-3080-2026-10-15); everything else is the
M4 Pro.

## GPU-driven rendering (area gpu-driven, 2026-10)

### Stress scene — `GpuDrivenTest.StressSceneTimings` (`OX_RENDER_PERF=<instances>`)

1080p, 200 m field, mixed primitives + an LOD sphere (every 8th instance), 64 materials (10 % alpha-tested), 12
occluding walls, sun with 4 cascades + 8 shadowed point lights (shadow caching off). Averages of 8 frames after
warm-up; run-to-run noise is about ±1 ms GPU at 50k.

**10 000 instances**

| config | GPU frame ms | CPU renderer ms | draw calls | indirect cmds | visible inst. |
|---|---|---|---|---|---|
| before: CPU culling + instanced (`r.GpuDriven 0`) | 9.38 | 3.04 | 1180 | 0 | 7331 |
| GPU culling (frustum + LOD) | 9.42 | 1.20 | 28 | 2254 | 7331 |
| GPU culling + two-phase HiZ (default) | **8.04** | **1.00** | 32 | 3536 | 2938 |
| + meshlets | 8.01 | 1.17 | 48 | 3552 | 2938 |

**50 000 instances**

| config | GPU frame ms | CPU renderer ms | draw calls | visible inst. | DepthPrepass | ForwardOpaque | Shadow.Cascades |
|---|---|---|---|---|---|---|---|
| before: CPU culling + instanced | 31.09 | 11.42 | 1418 | 36829 | 7.51 | 10.08 | 9.67 |
| GPU culling (frustum + LOD) | 31.68 | 1.95 | 28 | 36829 | 7.60 | 10.30 | 9.74 |
| GPU culling + two-phase HiZ (default) | **24.03** | **2.12** | 32 | 14601 | 3.45 (+0.03 late) | 6.60 | 9.74 |
| + meshlets | 23.87 | 2.20 | 48 | 14601 | 3.34 | 6.43 | 9.73 |

Culling cost at 50k: GpuCull 0.14 ms (all jobs: camera + 4 cascades + 8 point lights), HiZ.Early 0.09, GpuCull.Late
0.07 ms. CPU: the remaining 2 ms are `GpuScene::updateInstances` (≈ 2.0 ms at 50k, serial: next CPU hotspot) and graph
recording/submission (≈ 1.4 ms incl. MoltenVK encoding). Shadows are now the largest GPU cost (cascades rasterise
all casters inside each cascade; no HiZ for shadow views yet).

### Small scene — `RendererTest.PerfReport1080p` (400 instances, 64 lights)

| | GPU ms | CPU ms | draws |
|---|---|---|---|
| before (`r.GpuDriven 0`) | 3.51 | 0.80 | 548 |
| GPU-driven, first version (13 cull jobs × 4 dispatches, full-res HiZ.Early) | 3.92 | 0.60 | 16 |
| + half-resolution HiZ.Early (0.196 → 0.094 ms) | 3.77 | 0.64 | 16 |
| + one dispatch per stage for all jobs (GpuCull 0.133 → 0.022 ms) | 3.68 | 0.72 | 16 |

Little is occluded in this scene, so the two-phase pass costs ~0.15 ms net; it pays off from a few thousand
instances. CPU numbers at this size are within noise.

### Indirect draw count — `GpuDrivenTest.IndirectPaddingCost`

MoltenVK has no `drawIndirectCount`. 448 batches × LODs = 768 commands of which 21 are non-empty:

| | DepthPrepass | ForwardOpaque | API draw calls |
|---|---|---|---|
| CPU path (21 visible batches only) | 0.063–0.065 ms | 0.240–0.243 ms | 42 |
| GPU path, fixed max count with zero-instance padding | 0.073–0.087 ms | 0.255–0.298 ms | 2 |

≈ 20–60 ns per empty command. A count read back one frame late could only save this, but would drop newly visible
batches for a frame; **chosen: fixed max count** (`r.GpuDriven.DrawCount` Auto = drawIndirectCount with GPU-compacted
commands where supported, MaxCount otherwise).

### Async compute — `GpuDrivenTest.AsyncComputeOverlapsGraphics`

LightCulling + HiZ on the compute queue (render graph queue hints; the graph now splits batches so independent
graphics passes do not wait), 1280×720: GPU wall time sync 2.95–3.85 ms vs async 3.14–4.75 ms, measured overlap
0.000 ms. Metal serialises the queues (hazard tracking over the bindless heap) and the cross-queue semaphores add
latency, so `r.AsyncCompute` Auto = off on portability (MoltenVK) devices, on elsewhere. Async-safe passes: anything
whose resources are all declared to the graph (LightCulling, HiZ, feature passes using `asyncComputeHint`). Not
async: GpuCull / GpuCull.Late / HiZ.Early (untracked indirect buffers, critical path of the depth prepass).

### Parallel command recording — `GpuDrivenTest.ParallelRecordingProducesIdenticalImages`

3000 instances, 120 materials, CPU path, 1396 draws, 6 job threads: renderer CPU 1.67 ms serial vs 3.38 ms with
secondary command lists. MoltenVK records secondaries into its own command lists and re-encodes them serially into
Metal at submit, so it only adds work: `r.ParallelRecording` Auto = off on portability devices. Images identical
within frame-to-frame dither noise.

### Extract — `GpuDrivenTest.StressSceneTimings`

| meshes | serial | parallelFor (14 threads) |
|---|---|---|
| 10 013 | 0.14–0.30 ms | 0.14–1.30 ms |
| 50 013 | 0.72–0.94 ms | 0.51–0.84 ms |

Extract is memory bound; parallel only wins around 30–50k meshes → `ExtractOptions::parallelThreshold = 32768`.
(The first measurement of 7.8 ms for the parallel path was worker spin-up right after creating the job system.)

### Heap allocations — `GpuDrivenTest.SteadyStateExtractAndDrawListsDoNotAllocate`

Counting `operator new` in the test binary, 6000 instances, steady state: extract (serial and parallel below the
threshold) **0**, `updateInstances` + camera draw lists **0** (scratch and FrameState containers keep their capacity).
A whole renderer frame still makes ~1170 (CPU path) / ~1990 (GPU path) allocations, almost all from render graph
declarations (pass names, `std::function` captures, per-pass vectors) and `DrawList` vectors returned by
`buildDrawList` / `cullDrawList` — next target.

### Texture streaming — `GpuDrivenTest.TextureStreamingRespectsBudget`

24 × 1024² RGBA8 (128 MiB with mips), 720p, budget 4 MiB: wanted 12.6 MiB, resident 3.4 MiB, near texture 512 px,
far 64 px; raising the budget brings the near texture to full resolution within a few frames, no waits.

## NVIDIA RTX 3080 (2026-10)

Machine: Windows 11 Pro, NVIDIA GeForce RTX 3080 10 GB (driver 617.42, Vulkan 1.4), i9-9900K, 32 GB, MSVC 14.44,
RelWithDebInfo.

**Measurement conditions — read before comparing.** Validation was on, and during the session other builds kept the
CPU at about 90 % load and other processes shared the GPU. GPU times are usable; **CPU times are noisy** and only
show the trend. Nothing here was re-measured on an idle machine, at 4K or in Release. The exceptions, measured
separately with validation off at 1080p and 4K, are marked in their headings or first lines: the meshlet NTC decoder
table, the Showcase station table of "DLSS pass" (2026-10-15), "Ray tracing", "DLSS Ray Reconstruction" and
"Variable rate shading".

### Stress scene — `GpuDrivenTest.StressSceneTimings` (`OX_RENDER_PERF=<instances>`)

Same scene and settings as the M4 Pro tables above, 1080p.

| config | 10k: GPU frame ms | 10k: CPU renderer ms | 50k: GPU frame ms | 50k: CPU renderer ms |
|---|---|---|---|---|
| CPU culling + instanced (`r.GpuDriven 0`) | 2.34 | 7.1 | 9.8 | 29 |
| GPU culling (frustum + LOD) | 2.55 | 3.2 | 10.8 | 13 |
| GPU culling + two-phase HiZ (default) | 2.26 | 3.1 | 8.3 | 12.5 |
| + meshlets | 2.35 | 3.2 | 8.5 | 12.6 |

M4 Pro reference, default configuration (+ HiZ): 8.04 ms GPU at 10k and 24.03 ms at 50k.

CPU: `GpuScene::updateInstances` took 12.8 ms at 50k under load against 2.0 ms on the M4 Pro — the CPU hotspot on
Windows (`std::unordered_map` lookups), not optimised yet (BACKLOG). Two MSVC STL problems in the same function were
fixed during the bring-up: the `InstanceKey` hash `(entity << 16) ^ submesh` put every entity into one bucket, which
made `updateInstances` quadratic (177 ms at 10k, 2–3 ms after the fix), and a temporary `std::unordered_set`
allocated every frame even when empty (replaced by a sorted scratch vector).

### Mesh shaders and `drawIndirectCount`

Both paths are available on this GPU and really taken: the new counters `RenderStats::meshShaderDrawCalls` /
`indirectCountDrawCalls` are checked by the tests ("16 drawMeshTasksIndirect calls of 24", "indirect: 16 calls (16
with GPU draw count)"). The mesh shader image matches the compute meshlet path
(`GpuDrivenTest.MeshShaderPathCompilesAndMatchesWhenSupported`). The padded (fixed max count) and count variants of
the indirect draws cost the same on NVIDIA.

### Neural texture decoder off the meshlet path — `GpuDrivenTest.StressSceneTimings`

Validation off, `build/perf` (RelWithDebInfo), 2026-10-15, under the GPU lock. The meshlet pipelines (compute
expansion and `VK_EXT_mesh_shader`) compiled `depth_prepass.frag` / `forward.frag` with the NTC decoder in, like the
instanced path did before the neural area's fix (see [Neural rendering](#neural-rendering-rtx-3080-2026-10-14)). They
now keep two sets like the renderer: `OX_NTC_DISABLED` normally, the decoder set created on the first
`InferenceOnSample` frame. Stress scene (10 000 instances, GPU culling + HiZ + meshlets), `ForwardOpaque`, decoder
compiled in (`OX_RENDER_PERF_CVARS=r.NeuralTextures=InferenceOnSample`, the pipelines the meshlet path used before
for every material) → `OX_NTC_DISABLED` set (`r.NeuralTextures=Off`):

| resolution | ForwardOpaque decoder in, ms | ForwardOpaque `OX_NTC_DISABLED`, ms | GPU frame, ms |
|---|---|---|---|
| 1080p | 0.88–0.95 | 0.46–0.51 | 2.76–2.89 → 2.27–2.41 |
| 4K | 2.52 (4.28 in a contended run) | 0.83–0.90 | 5.07 → 3.17–3.52 |

`OX_RENDER_PERF_RES=<w>x<h>` and `OX_RENDER_PERF_CVARS=name=value,...` are new options of the test.

### Async compute and parallel recording — `GpuDrivenTest.StressSceneAsyncComputeAndParallelRecording`

| | off | on |
|---|---|---|
| `r.AsyncCompute`, GPU wall ms, 10k | 2.22 | 2.19 |
| `r.AsyncCompute`, GPU wall ms, 50k (three runs) | 8.06 / 8.31 / 8.65 | 7.99 / 8.19 / 8.19 |
| `r.ParallelRecording`, CPU renderer ms, validation off | 2.07 | 2.14 |
| `r.ParallelRecording`, CPU renderer ms, validation on | 8.0 | 5.4 |

Async compute: a small but consistent gain. The overlap is real here, 0.075 of 0.076 ms of async compute in the
stress scene (0.000 ms on MoltenVK), after three fixes in the render graph (queue-family-aware barrier stages, batch
split on any cross-queue access, back-to-back submission — see `modules/rhi.md`). `Auto` already turns it on outside
MoltenVK; the heuristic was not changed.

Parallel recording: no gain without validation, a gain with it. Not conclusive under load, so `Auto` was not changed.

### DLSS pass

GPU time of the `DLSS` pass at 1080p: Performance 6.2 ms, Quality 3.95 ms, DLAA 2.9 ms. Rough numbers (same
conditions as above). Render resolutions per mode and the image quality checks are in
`modules/render_postprocess.md`.

Showcase stations (headless player, quality High, ray tracing off, validation off, `build/perf`, under the GPU
lock), median GPU frame time in ms over frames 61–300 (`--pass-stats`), 2026-10-15:

| Mode | Station 4 (volumetrics) 1080p | Station 4 4K | Station 6 (world) 1080p | Station 6 4K |
| --- | --- | --- | --- | --- |
| Off (native) | 3.57 | 8.42 (12.94 before `*.HalfResAboveHeight`) | 2.60 | 7.46 |
| FSR 1 Quality | — | — | 1.64 | 5.21 |
| TAAU Quality | — | — | 2.18 | 6.68 |

The 2026-10-14 table (station 4 "37–39 ms", station 6 FSR 1 / TAAU "14 or 50–59 ms") is not reproducible on
the current tree. Station 6 was the overlay's value of the *last* frame. Per-frame GPU times (`--pass-csv`) show
what makes single frames 5–10× slower: one pass of the frame (any pass: `FSR1.EASU` 37.6 ms in a frame whose other
passes were normal, `AO.Trace` 8.6 ms in another) absorbs a time slice of another process on the same GPU. The
RTX 3080 is shared with the Android emulator (gfxstream, `qemu-system-x86_64-headless`), the other agents' GPU
tests and the browser; Windows time-slices GPU contexts, and the engine's per-pass timestamps
(`ALL_COMMANDS` before and after a pass) include the slice. With the GPU lock and no other GPU test running, FSR 1 /
TAAU at 4K are stable (FSR 1: p10 4.95, median 5.21, p90 6.01 ms; TAAU: 6.26 / 6.68 / 7.67), threaded and
`--single-thread` alike; the first second of a run (pipeline compiles, texture streaming) reads 30–40 ms in the
`--stats` windows. Nothing in the upscalers re-creates resources or pipelines per frame (both upscalers are two
fixed compute passes on graph-transient textures, histories persistent). Measure with the median of
`--pass-stats`, not one frame. Reproduce: `OxwaldPlayer --project samples/OxwaldShowcase --headless --width 3840
--height 2160 --quality high --scene project://Assets/Scenes/Stations/06_World.oxscene --frames 300 --cvar
r.AntiAliasing=2 --cvar r.Upscaler=1 --cvar r.Upscaler.Quality=3 --pass-stats --pass-csv frames.csv -- --photo`.

Station 4 at native 4K: the volumetrics are not what grows with the resolution. All `Volumetrics.*` passes take
0.65 ms at 1080p and 0.8–1.2 ms at 4K (froxel grid 160×90×64 independent of the output; clouds at 1/2 resolution
with 1/16 checkerboard updates; the composite is one fullscreen pass, 0.13 ms at 4K). The 4K frame was
screen-space AO (`AO.Trace` 2.34 ms) and SSR (`Reflections.SSRTrace` 2.11 + `SSRResolve` 1.11 ms), traced at full
resolution on High / Ultra. New `r.AO.HalfResAboveHeight` / `r.SSR.HalfResAboveHeight` (default 1800) trace them
at half resolution from a render height of 1800 px: at native 4K the trace keeps 1080p density, the bilateral
upsample / resolve keep the image. Station 4, 4K, High: 12.94 → 8.42 ms (AO.Trace 2.34 → 0.71, SSRTrace 2.11 →
0.58, SSRResolve 1.11 → 0.49 ms); screenshot PSNR against full resolution 40.93 dB, SSIM 0.9932 (run-to-run noise
of two full-resolution runs: 39.83 dB, SSIM 0.9978; the crops are visually identical). Upscaled 4K (render
height 1440) and 1080p / 1440p are unchanged.

### Texture streaming — `GpuDrivenTest.TextureStreamingRespectsBudget`

Residency is the same as on the M4 Pro (near texture 512 px, far 64 px). NVIDIA selects LODs differently with 16×
anisotropy, so the image differs from the golden in 3.66 % of the pixels; the test's tolerance is 5 % and the golden
was not regenerated.

### Ray tracing — `RayTracingTest.PerfReportRayTracing` (`OX_RT_PERF=1 OX_VULKAN_VALIDATION=0`)

Validation **off**, RelWithDebInfo, default cvars, no upscaler; one run on 2026-10-14 (the totals agree with the
first measurement of the session: 9.1 ms and 31 ms of ray tracing passes). Scene: 240 cubes / spheres / cylinders on
a 120 m plane, a mirror-like floor patch, 4 glass spheres, 8 shadowed point lights, sun, height fog. 48 warm-up
frames, then the average of 8 frames with one moving instance. `OX_RT_PERF=<width>x<height>` measures one custom
size.

| configuration | 1080p: GPU frame ms | 1080p: RT passes ms | 1080p: VRAM MB | 4K: GPU frame ms | 4K: RT passes ms | 4K: VRAM MB |
|---|---|---|---|---|---|---|
| raster | 1.51 | — | 810 | 3.50 | — | 1959 |
| ray tracing (default) | 9.84 | 9.18 | 1398 | 32.88 | 31.24 | 3555 |
| ray tracing + ReSTIR | 9.67 | 9.04 | 1588 | 33.69 | 32.02 | 4124 |
| path tracer, mode 0 (ray query) | 10.57 | 9.95 | 1622 | 37.16 | 35.44 | 4218 |
| path tracer, mode 1 (RT pipeline) | 11.14 | 10.46 | 1622 | 38.69 | 36.87 | 4218 |

VRAM is the device total (`GpuMemoryStats::totalUsageBytes`) of the test process; the configurations run one after
another in the same process, so later rows may include resources of earlier ones.

Per pass, "ray tracing (default)"; a denoiser row is the sum of all denoiser stages of that signal:

| pass | 1080p ms | 4K ms |
|---|---|---|
| RT.BuildAS (TLAS update) | 0.01 | 0.01 |
| RT.Shadows | 1.08 | 4.41 |
| RtShadowSun (denoise) | 1.49 | 5.77 |
| RtShadowLocal (denoise) | 1.55 | 5.70 |
| RT.Shadows.PublishLocal | 0.02 | 0.06 |
| RT.DDGI.Trace | 0.25 | 0.25 |
| RT.DDGI.UpdateIrradiance | 0.15 | 0.27 |
| RT.DDGI.UpdateDepth | 0.69 | 0.63 |
| RT.DDGI.Border | 0.03 | 0.03 |
| RT.DDGI.Apply | 0.25 | 0.88 |
| RT.AO | 0.09 | 0.35 |
| RtAO (denoise) | 1.52 | 5.68 |
| RT.Reflections | 0.22 | 0.82 |
| RtReflections (denoise) | 1.38 | 5.36 |
| RT.VolumetricVisibility | 0.22 | 0.18 |
| RT.Translucency | 0.23 | 0.81 |
| **sum** | **9.18** | **31.24** |

With ReSTIR: RT.ReSTIR.Initial 0.64 / 2.53 ms, RT.ReSTIR.Spatial 0.37 / 1.23 ms. Path tracer at 1 sample per
frame: RT.PathTrace 1.13 / 4.45 ms (mode 0), RT.PathTrace.Pipeline 1.46 / 5.84 ms (mode 1).

**Path tracer no longer pays for the hybrid effects (2026-10-15).** The hybrid passes (shadows, AO, GI,
reflections, volumetric visibility, refraction, their SVGF stages) kept running under `r.PathTracing` although the
path traced image replaces SceneColorHDR. Now `RtFeatureBase::gather()` returns false for every hybrid effect while
`rt::pathTracerActive()`: no pass, no target (test `RayTracingTest.HybridEffectsSkipUnderPathTracer`; 32 textures
/ 470 MB under the path tracer against 99 / 610 MB with the hybrid effects on its test scene). The path tracer rows
of the table above are the "before" numbers.

| path tracer | 1080p GPU frame before → after, ms | 4K GPU frame before → after, ms |
|---|---|---|
| mode 0 (ray query) | 10.28 → 1.67 | 36.06 → 5.63 |
| mode 1 (RT pipeline) | 10.37 → 2.17 | 36.01 → 7.03 |

"Before" is the `build/dev` binary of the same day (same test, same machine); RT passes under the path tracer
dropped from 9.50 to 1.05 ms (1080p) and from 34.10 to 4.09 ms (4K): only RT.BuildAS + RT.PathTrace remain.

The four denoised signals take 5.9 of 9.2 ms at 1080p and 22.5 of 31.2 ms at 4K: the SVGF denoiser is the cost, the
rays are not. `DenoiserSettings::clampHistory` (shadow signals) added 0.06 ms per signal at 1080p. The DDGI probe
passes and the froxel visibility do not scale with the resolution.

### DLSS Ray Reconstruction — `RayReconstructionTest.PerfReportRayReconstruction` (`OX_RT_PERF=1 OX_VULKAN_VALIDATION=0`)

Validation **off**, RelWithDebInfo, driver 617.42, one run on 2026-10-14 under the GPU lock. Scene of
`RayTracingTest.PerfReportRayTracing` (240 objects, mirror-like patch, glass, 8 shadowed point lights, sun, fog);
48 warm-up frames, then the average of 8 frames with one moving instance; each configuration starts with a fresh
view. "RT passes" are all `RT.*` / `Rt*` passes (under RR the SVGF stages are skipped, only the format resolve /
half-res upsample and `RT.RRGuides` remain); "upscaler pass" is `DLSS` or `DLSSRR`. RR runs preset E.

| Output | Configuration | Render | GPU frame ms | RT passes ms | of it denoise / resolve | RR guides | Upscaler pass ms |
|---|---|---|---|---|---|---|---|
| 1080p | SVGF, native, no AA | 1920×1080 | 9.28 | 8.16 | 5.21 | — | — |
| 1080p | SVGF + DLSS Quality | 1280×720 | 5.95 | 4.53 | 2.46 | — | 0.74 |
| 1080p | RR Quality | 1280×720 | 7.18 | 2.20 | 0.04 | 0.07 | 4.28 |
| 1080p | SVGF + DLSS Performance | 960×540 | 5.06 | 3.24 | 1.48 | — | 1.30 |
| 1080p | RR Performance | 960×540 | 4.70 | 1.77 | 0.02 | 0.04 | 2.43 |
| 1080p | SVGF + DLAA | 1920×1080 | 9.97 | 8.11 | 5.20 | — | 0.74 |
| 1080p | RR DLAA | 1920×1080 | 13.74 | 3.49 | 0.36 | 0.16 | 9.12 |
| 4K | SVGF, native, no AA | 3840×2160 | 33.37 | 29.85 | 20.70 | — | — |
| 4K | SVGF + DLSS Quality | 2560×1440 | 17.53 | 13.47 | 9.24 | — | 2.17 |
| 4K | RR Quality | 2560×1440 | 24.47 | 6.01 | 0.41 | 0.27 | 16.51 |
| 4K | SVGF + DLSS Performance | 1920×1080 | 14.34 | 8.32 | 5.30 | — | 4.78 |
| 4K | RR Performance | 1920×1080 | 13.52 | 3.24 | 0.07 | 0.16 | 9.05 |
| 4K | SVGF + DLAA | 3840×2160 | 35.17 | 29.32 | 21.26 | — | 2.25 |
| 4K | RR DLAA | 3840×2160 | 49.02 | 9.21 | 0.27 | 0.61 | 36.16 |

On the RTX 3080 Ray Reconstruction saves the SVGF stages but its network costs more than they do, except in
Performance mode (−0.36 ms at 1080p, −0.82 ms at 4K). It is a quality option on this GPU, not a speed-up. The
default preset (F) instead of E: RR pass 4.10 / 2.45 / 8.16 ms at 1080p Quality / Performance / DLAA and 14.60 /
9.32 / 31.24 ms at 4K (4–15 % cheaper, 0.2 dB further from the path tracer, more flicker at DLAA).

VRAM: the first RR frame raises the driver-reported usage by ~180–510 MB (varied between runs); NGX keeps it after
the RR feature is released until NGX shuts down with the device. Repeated RR ↔ SVGF toggling does not grow it
(`RuntimeToggleWithoutLeaks`).

Image quality (`ImageQualityAndStabilityVersusSvgfPlusDlss`, 640×360, half-resolution PSNR):

| Configuration | vs path tracer (4096 spp) | vs converged hybrid | flicker |
|---|---|---|---|
| SVGF + DLSS Quality | 25.81 dB | 34.54 dB | 0.037 |
| RR Quality | 25.32 dB | 37.82 dB | 0.089 |
| SVGF + DLAA | 25.67 dB | 37.12 dB | 0.031 |
| RR DLAA | 25.32 dB | 38.19 dB | 0.045 |

Why RR is lower against the path tracer but higher against the converged hybrid frame: see
`modules/render_postprocess.md`, "DLSS Ray Reconstruction".

### Variable rate shading — `ShowcaseVrs.StationsQualityAndPerformance` (`OX_VRS_PERF=1 OX_VULKAN_VALIDATION=0`)

`ox_showcase_tests`, RelWithDebInfo, validation off, 2026-10-14 23:53–00:12 under the GPU lock (other agents'
functional GPU tests may have run at the same time). Every row is a fresh headless engine: station in photo mode at
quality High with the VRS cvars overridden, fixed 1/60 s step, medians of frames 121–180 (GPU frame, the passes that
get the rate image = "affected", the VRS passes), quality of frame 181 against the r.VRS-off frame of the same set
(HUD FPS counter masked). TAA on. "pan" = camera yawing 0.6°/frame from frame 60, compared with the panning Off run.
τ = `r.VRS.Threshold`, 2×2 / 4×4 = `r.VRS.MaxRate`. Presets: Low = τ 0.05, 4×4; Medium = τ 0.03, 2×2; High / Ultra
off (τ 0.02, 2×2 if switched on). Outliers: 06_World 1080p Off frame (2.85 ms against 2.61 in the repeat) and the
02_Materials 1080p Foveated run (a 5 ms hitch, VRS passes 0.18 ms) — read the affected-pass column there.
Reproduce: `OX_VRS_PERF=1 OX_VULKAN_VALIDATION=0 OX_VRS_OUT=E:/... ox_showcase_tests --gtest_filter=ShowcaseVrs.*`
(~20 min, ~1 GB of PNGs); raw CSV of this run: `E:/Development/OxwaldEngine/vrs_perf/results_v4.csv`.

| station | size | configuration | GPU frame ms | VRS passes ms | affected passes ms | coarse tiles | PSNR dB | SSIM | FLIP |
|---|---|---|---|---|---|---|---|---|---|
| 01_Lighting | 1080p | Off | 2.75 | — | 0.68 | — | — | — | — |
| 01_Lighting | 1080p | Off again (noise) | 2.75 (-0 %) | 0.00 | 0.67 (-0.00) | 0 % | ∞ | 1.0000 | 0.0000 |
| 01_Lighting | 1080p | τ 0.02, 2×2 (High/Ultra if on) | 2.66 (-3 %) | 0.04 | 0.54 (-0.13) | 73 % | 46.9 | 0.9953 | 0.0086 |
| 01_Lighting | 1080p | τ 0.03, 2×2 (**Medium**) | 2.62 (-5 %) | 0.04 | 0.51 (-0.17) | 81 % | 46.4 | 0.9946 | 0.0093 |
| 01_Lighting | 1080p | τ 0.05, 2×2 | 2.61 (-5 %) | 0.04 | 0.49 (-0.18) | 84 % | 45.8 | 0.9935 | 0.0102 |
| 01_Lighting | 1080p | τ 0.05, 4×4 (**Low**) | 2.58 (-6 %) | 0.04 | 0.46 (-0.21) | 84 % | 41.1 | 0.9872 | 0.0158 |
| 01_Lighting | 1080p | τ 0.08, 2×2 (old High) | 2.58 (-6 %) | 0.04 | 0.48 (-0.19) | 84 % | 45.2 | 0.9926 | 0.0106 |
| 01_Lighting | 1080p | Motion, 4×4 | 2.77 (+1 %) | 0.04 | 0.67 (-0.01) | 0 % | ∞ | 1.0000 | 0.0000 |
| 01_Lighting | 1080p | Foveated, 4×4 | 2.69 (-2 %) | 0.04 | 0.57 (-0.10) | 55 % | 42.9 | 0.9928 | 0.0111 |
| 01_Lighting | 1080p | pan: Off | 2.58 | — | 0.53 | — | — | — | — |
| 01_Lighting | 1080p | pan: τ 0.03, 2×2 | 2.42 (-6 %) | 0.04 | 0.33 (-0.20) | 85 % | 51.2 | 0.9985 | 0.0091 |
| 01_Lighting | 1080p | pan: Motion, 4×4 | 2.45 (-5 %) | 0.04 | 0.35 (-0.18) | 69 % | 55.6 | 0.9991 | 0.0084 |
| 01_Lighting | 4K | Off | 7.63 | — | 1.98 | — | — | — | — |
| 01_Lighting | 4K | Off again (noise) | 7.69 (+1 %) | 0.00 | 1.99 (+0.01) | 0 % | ∞ | 1.0000 | 0.0000 |
| 01_Lighting | 4K | τ 0.02, 2×2 (High/Ultra if on) | 7.11 (-7 %) | 0.13 | 1.36 (-0.62) | 81 % | 49.8 | 0.9972 | 0.0066 |
| 01_Lighting | 4K | τ 0.03, 2×2 (**Medium**) | 7.04 (-8 %) | 0.13 | 1.30 (-0.68) | 84 % | 49.3 | 0.9966 | 0.0071 |
| 01_Lighting | 4K | τ 0.05, 2×2 | 7.02 (-8 %) | 0.13 | 1.24 (-0.74) | 85 % | 48.5 | 0.9955 | 0.0078 |
| 01_Lighting | 4K | τ 0.05, 4×4 (**Low**) | 7.03 (-8 %) | 0.13 | 1.13 (-0.86) | 85 % | 43.4 | 0.9903 | 0.0129 |
| 01_Lighting | 4K | τ 0.08, 2×2 (old High) | 6.96 (-9 %) | 0.13 | 1.24 (-0.75) | 86 % | 48.1 | 0.9952 | 0.0080 |
| 01_Lighting | 4K | Motion, 4×4 | 7.84 (+3 %) | 0.13 | 1.98 (-0.01) | 0 % | ∞ | 1.0000 | 0.0000 |
| 01_Lighting | 4K | Foveated, 4×4 | 7.35 (-4 %) | 0.13 | 1.65 (-0.33) | 54 % | 45.8 | 0.9954 | 0.0076 |
| 01_Lighting | 4K | pan: Off | 7.47 | — | 1.64 | — | — | — | — |
| 01_Lighting | 4K | pan: τ 0.03, 2×2 | 6.88 (-8 %) | 0.13 | 0.97 (-0.68) | 87 % | 54.8 | 0.9990 | 0.0071 |
| 01_Lighting | 4K | pan: Motion, 4×4 | 6.91 (-7 %) | 0.13 | 1.03 (-0.62) | 69 % | 56.7 | 0.9993 | 0.0071 |
| 02_Materials | 1080p | Off | 2.56 | — | 0.21 | — | — | — | — |
| 02_Materials | 1080p | Off again (noise) | 2.57 (+0 %) | 0.00 | 0.21 (+0.00) | 0 % | 66.6 | 0.9999 | 0.0015 |
| 02_Materials | 1080p | τ 0.02, 2×2 (High/Ultra if on) | 2.60 (+2 %) | 0.07 | 0.20 (-0.01) | 32 % | 56.4 | 0.9991 | 0.0044 |
| 02_Materials | 1080p | τ 0.03, 2×2 (**Medium**) | 2.59 (+1 %) | 0.07 | 0.19 (-0.02) | 45 % | 48.9 | 0.9951 | 0.0076 |
| 02_Materials | 1080p | τ 0.05, 2×2 | 2.56 (-0 %) | 0.07 | 0.16 (-0.05) | 69 % | 41.1 | 0.9742 | 0.0173 |
| 02_Materials | 1080p | τ 0.05, 4×4 (**Low**) | 2.56 (-0 %) | 0.07 | 0.16 (-0.05) | 69 % | 39.9 | 0.9636 | 0.0206 |
| 02_Materials | 1080p | τ 0.08, 2×2 (old High) | 2.54 (-1 %) | 0.07 | 0.15 (-0.07) | 81 % | 37.8 | 0.9536 | 0.0254 |
| 02_Materials | 1080p | Motion, 4×4 | 2.64 (+3 %) | 0.07 | 0.21 (-0.00) | 0 % | 68.6 | 1.0000 | 0.0009 |
| 02_Materials | 1080p | Foveated, 4×4 | 7.86 (+207 %) | 0.18 | 0.43 (+0.21) | 55 % | 34.8 | 0.9450 | 0.0250 |
| 02_Materials | 1080p | pan: Off | 2.29 | — | 0.15 | — | — | — | — |
| 02_Materials | 1080p | pan: τ 0.03, 2×2 | 2.29 (+0 %) | 0.04 | 0.10 (-0.05) | 81 % | 45.5 | 0.9894 | 0.0190 |
| 02_Materials | 1080p | pan: Motion, 4×4 | 2.25 (-2 %) | 0.04 | 0.10 (-0.05) | 49 % | 51.4 | 0.9979 | 0.0132 |
| 02_Materials | 4K | Off | 6.68 | — | 0.70 | — | — | — | — |
| 02_Materials | 4K | Off again (noise) | 6.58 (-2 %) | 0.00 | 0.69 (-0.01) | 0 % | ∞ | 1.0000 | 0.0000 |
| 02_Materials | 4K | τ 0.02, 2×2 (High/Ultra if on) | 6.73 (+1 %) | 0.24 | 0.59 (-0.11) | 51 % | 53.1 | 0.9977 | 0.0066 |
| 02_Materials | 4K | τ 0.03, 2×2 (**Medium**) | 6.72 (+0 %) | 0.24 | 0.53 (-0.17) | 65 % | 48.1 | 0.9934 | 0.0097 |
| 02_Materials | 4K | τ 0.05, 2×2 | 6.54 (-2 %) | 0.24 | 0.45 (-0.25) | 80 % | 42.9 | 0.9820 | 0.0152 |
| 02_Materials | 4K | τ 0.05, 4×4 (**Low**) | 6.56 (-2 %) | 0.23 | 0.42 (-0.28) | 80 % | 40.8 | 0.9677 | 0.0218 |
| 02_Materials | 4K | τ 0.08, 2×2 (old High) | 6.61 (-1 %) | 0.24 | 0.41 (-0.29) | 85 % | 40.3 | 0.9723 | 0.0195 |
| 02_Materials | 4K | Motion, 4×4 | 6.91 (+3 %) | 0.24 | 0.70 (-0.00) | 0 % | 74.7 | 1.0000 | 0.0002 |
| 02_Materials | 4K | Foveated, 4×4 | 6.67 (-0 %) | 0.24 | 0.48 (-0.22) | 54 % | 37.4 | 0.9637 | 0.0187 |
| 02_Materials | 4K | pan: Off | 6.22 | — | 0.53 | — | — | — | — |
| 02_Materials | 4K | pan: τ 0.03, 2×2 | 6.02 (-3 %) | 0.13 | 0.30 (-0.23) | 86 % | 47.4 | 0.9926 | 0.0156 |
| 02_Materials | 4K | pan: Motion, 4×4 | 6.08 (-2 %) | 0.13 | 0.30 (-0.23) | 48 % | 50.2 | 0.9963 | 0.0126 |
| 04_Volumetrics | 1080p | Off | 3.57 | — | 0.37 | — | — | — | — |
| 04_Volumetrics | 1080p | Off again (noise) | 3.55 (-1 %) | 0.00 | 0.37 (-0.00) | 0 % | ∞ | 1.0000 | 0.0000 |
| 04_Volumetrics | 1080p | τ 0.02, 2×2 (High/Ultra if on) | 3.45 (-3 %) | 0.04 | 0.22 (-0.14) | 82 % | 52.0 | 0.9972 | 0.0085 |
| 04_Volumetrics | 1080p | τ 0.03, 2×2 (**Medium**) | 3.45 (-3 %) | 0.04 | 0.22 (-0.15) | 85 % | 49.9 | 0.9960 | 0.0094 |
| 04_Volumetrics | 1080p | τ 0.05, 2×2 | 3.43 (-4 %) | 0.04 | 0.21 (-0.16) | 87 % | 46.2 | 0.9930 | 0.0112 |
| 04_Volumetrics | 1080p | τ 0.05, 4×4 (**Low**) | 3.41 (-4 %) | 0.04 | 0.19 (-0.18) | 87 % | 43.9 | 0.9872 | 0.0157 |
| 04_Volumetrics | 1080p | τ 0.08, 2×2 (old High) | 3.47 (-3 %) | 0.04 | 0.21 (-0.16) | 87 % | 43.4 | 0.9904 | 0.0130 |
| 04_Volumetrics | 1080p | Motion, 4×4 | 3.91 (+10 %) | 0.04 | 0.36 (-0.01) | 0 % | ∞ | 1.0000 | 0.0000 |
| 04_Volumetrics | 1080p | Foveated, 4×4 | 3.49 (-2 %) | 0.04 | 0.25 (-0.11) | 55 % | 39.1 | 0.9863 | 0.0139 |
| 04_Volumetrics | 1080p | pan: Off | 3.27 | — | 0.26 | — | — | — | — |
| 04_Volumetrics | 1080p | pan: τ 0.03, 2×2 | 3.20 (-2 %) | 0.04 | 0.15 (-0.12) | 86 % | 49.3 | 0.9977 | 0.0102 |
| 04_Volumetrics | 1080p | pan: Motion, 4×4 | 3.24 (-1 %) | 0.04 | 0.17 (-0.09) | 68 % | 49.2 | 0.9989 | 0.0091 |
| 04_Volumetrics | 4K | Off | 8.61 | — | 1.26 | — | — | — | — |
| 04_Volumetrics | 4K | Off again (noise) | 8.53 (-1 %) | 0.00 | 1.26 (+0.00) | 0 % | ∞ | 1.0000 | 0.0000 |
| 04_Volumetrics | 4K | τ 0.02, 2×2 (High/Ultra if on) | 8.76 (+2 %) | 0.14 | 0.65 (-0.61) | 86 % | 53.6 | 0.9976 | 0.0074 |
| 04_Volumetrics | 4K | τ 0.03, 2×2 (**Medium**) | 7.98 (-7 %) | 0.14 | 0.64 (-0.62) | 87 % | 51.8 | 0.9968 | 0.0081 |
| 04_Volumetrics | 4K | τ 0.05, 2×2 | 7.92 (-8 %) | 0.14 | 0.63 (-0.64) | 87 % | 49.6 | 0.9958 | 0.0089 |
| 04_Volumetrics | 4K | τ 0.05, 4×4 (**Low**) | 7.91 (-8 %) | 0.14 | 0.53 (-0.74) | 87 % | 46.4 | 0.9908 | 0.0125 |
| 04_Volumetrics | 4K | τ 0.08, 2×2 (old High) | 7.87 (-9 %) | 0.14 | 0.62 (-0.65) | 87 % | 47.5 | 0.9950 | 0.0097 |
| 04_Volumetrics | 4K | Motion, 4×4 | 8.55 (-1 %) | 0.14 | 1.24 (-0.02) | 0 % | ∞ | 1.0000 | 0.0000 |
| 04_Volumetrics | 4K | Foveated, 4×4 | 8.27 (-4 %) | 0.14 | 0.82 (-0.44) | 54 % | 43.7 | 0.9930 | 0.0093 |
| 04_Volumetrics | 4K | pan: Off | 8.15 | — | 1.00 | — | — | — | — |
| 04_Volumetrics | 4K | pan: τ 0.03, 2×2 | 7.70 (-6 %) | 0.13 | 0.50 (-0.51) | 87 % | 51.4 | 0.9986 | 0.0070 |
| 04_Volumetrics | 4K | pan: Motion, 4×4 | 7.75 (-5 %) | 0.13 | 0.56 (-0.45) | 67 % | 52.6 | 0.9994 | 0.0060 |
| 05_Water | 1080p | Off | 2.85 | — | 0.68 | — | — | — | — |
| 05_Water | 1080p | Off again (noise) | 2.85 (+0 %) | 0.00 | 0.68 (+0.00) | 0 % | 72.4 | 1.0000 | 0.0007 |
| 05_Water | 1080p | τ 0.02, 2×2 (High/Ultra if on) | 2.83 (-0 %) | 0.07 | 0.59 (-0.09) | 51 % | 54.8 | 0.9986 | 0.0062 |
| 05_Water | 1080p | τ 0.03, 2×2 (**Medium**) | 2.78 (-2 %) | 0.07 | 0.54 (-0.14) | 63 % | 50.1 | 0.9975 | 0.0088 |
| 05_Water | 1080p | τ 0.05, 2×2 | 2.72 (-5 %) | 0.07 | 0.48 (-0.19) | 74 % | 45.6 | 0.9947 | 0.0118 |
| 05_Water | 1080p | τ 0.05, 4×4 (**Low**) | 2.71 (-5 %) | 0.07 | 0.47 (-0.20) | 74 % | 44.6 | 0.9921 | 0.0141 |
| 05_Water | 1080p | τ 0.08, 2×2 (old High) | 2.68 (-6 %) | 0.07 | 0.44 (-0.23) | 82 % | 41.8 | 0.9898 | 0.0161 |
| 05_Water | 1080p | Motion, 4×4 | 2.94 (+3 %) | 0.07 | 0.68 (+0.01) | 0 % | 82.9 | 1.0000 | 0.0000 |
| 05_Water | 1080p | Foveated, 4×4 | 2.73 (-4 %) | 0.07 | 0.48 (-0.20) | 55 % | 33.2 | 0.9624 | 0.0247 |
| 05_Water | 1080p | pan: Off | 3.15 | — | 0.67 | — | — | — | — |
| 05_Water | 1080p | pan: τ 0.03, 2×2 | 2.99 (-5 %) | 0.07 | 0.47 (-0.21) | 80 % | 46.5 | 0.9973 | 0.0149 |
| 05_Water | 1080p | pan: Motion, 4×4 | 2.97 (-6 %) | 0.07 | 0.43 (-0.25) | 63 % | 45.7 | 0.9975 | 0.0149 |
| 05_Water | 4K | Off | 8.37 | — | 2.36 | — | — | — | — |
| 05_Water | 4K | Off again (noise) | 8.29 (-1 %) | 0.00 | 2.38 (+0.02) | 0 % | 84.5 | 1.0000 | 0.0000 |
| 05_Water | 4K | τ 0.02, 2×2 (High/Ultra if on) | 7.91 (-6 %) | 0.24 | 1.74 (-0.62) | 66 % | 53.4 | 0.9983 | 0.0068 |
| 05_Water | 4K | τ 0.03, 2×2 (**Medium**) | 7.62 (-9 %) | 0.24 | 1.56 (-0.80) | 74 % | 50.1 | 0.9971 | 0.0082 |
| 05_Water | 4K | τ 0.05, 2×2 | 7.53 (-10 %) | 0.24 | 1.42 (-0.94) | 81 % | 46.3 | 0.9943 | 0.0102 |
| 05_Water | 4K | τ 0.05, 4×4 (**Low**) | 7.91 (-6 %) | 0.23 | 1.31 (-1.05) | 81 % | 44.9 | 0.9908 | 0.0139 |
| 05_Water | 4K | τ 0.08, 2×2 (old High) | 7.54 (-10 %) | 0.24 | 1.30 (-1.06) | 86 % | 43.0 | 0.9905 | 0.0129 |
| 05_Water | 4K | Motion, 4×4 | 8.79 (+5 %) | 0.24 | 2.34 (-0.02) | 0 % | 80.5 | 1.0000 | 0.0001 |
| 05_Water | 4K | Foveated, 4×4 | 7.86 (-6 %) | 0.24 | 1.52 (-0.84) | 54 % | 35.6 | 0.9769 | 0.0173 |
| 05_Water | 4K | pan: Off | 9.23 | — | 2.29 | — | — | — | — |
| 05_Water | 4K | pan: τ 0.03, 2×2 | 8.53 (-8 %) | 0.24 | 1.32 (-0.97) | 86 % | 50.9 | 0.9988 | 0.0110 |
| 05_Water | 4K | pan: Motion, 4×4 | 8.21 (-11 %) | 0.24 | 1.19 (-1.10) | 63 % | 49.3 | 0.9987 | 0.0112 |
| 06_World | 1080p | Off | 2.85 | — | 0.60 | — | — | — | — |
| 06_World | 1080p | Off again (noise) | 2.61 (-9 %) | 0.00 | 0.59 (-0.00) | 0 % | 80.2 | 1.0000 | 0.0000 |
| 06_World | 1080p | τ 0.02, 2×2 (High/Ultra if on) | 2.60 (-9 %) | 0.04 | 0.54 (-0.06) | 34 % | 51.9 | 0.9962 | 0.0068 |
| 06_World | 1080p | τ 0.03, 2×2 (**Medium**) | 2.56 (-10 %) | 0.04 | 0.50 (-0.10) | 48 % | 46.7 | 0.9904 | 0.0114 |
| 06_World | 1080p | τ 0.05, 2×2 | 2.51 (-12 %) | 0.04 | 0.44 (-0.16) | 64 % | 41.6 | 0.9799 | 0.0188 |
| 06_World | 1080p | τ 0.05, 4×4 (**Low**) | 2.47 (-13 %) | 0.04 | 0.42 (-0.18) | 64 % | 39.8 | 0.9663 | 0.0246 |
| 06_World | 1080p | τ 0.08, 2×2 (old High) | 2.45 (-14 %) | 0.04 | 0.40 (-0.20) | 76 % | 38.9 | 0.9711 | 0.0264 |
| 06_World | 1080p | Motion, 4×4 | 2.68 (-6 %) | 0.04 | 0.60 (+0.01) | 0 % | 86.7 | 1.0000 | 0.0000 |
| 06_World | 1080p | Foveated, 4×4 | 2.48 (-13 %) | 0.04 | 0.43 (-0.17) | 55 % | 35.2 | 0.9602 | 0.0297 |
| 06_World | 1080p | pan: Off | 2.77 | — | 0.56 | — | — | — | — |
| 06_World | 1080p | pan: τ 0.03, 2×2 | 2.61 (-6 %) | 0.04 | 0.37 (-0.19) | 79 % | 44.9 | 0.9939 | 0.0285 |
| 06_World | 1080p | pan: Motion, 4×4 | 2.62 (-5 %) | 0.04 | 0.38 (-0.18) | 81 % | 41.5 | 0.9939 | 0.0321 |
| 06_World | 4K | Off | 7.44 | — | 1.94 | — | — | — | — |
| 06_World | 4K | Off again (noise) | 7.53 (+1 %) | 0.00 | 1.95 (+0.01) | 0 % | ∞ | 1.0000 | 0.0000 |
| 06_World | 4K | τ 0.02, 2×2 (High/Ultra if on) | 7.13 (-4 %) | 0.13 | 1.53 (-0.42) | 50 % | 48.2 | 0.9903 | 0.0110 |
| 06_World | 4K | τ 0.03, 2×2 (**Medium**) | 6.98 (-6 %) | 0.13 | 1.35 (-0.60) | 64 % | 44.6 | 0.9822 | 0.0160 |
| 06_World | 4K | τ 0.05, 2×2 | 6.79 (-9 %) | 0.13 | 1.21 (-0.73) | 74 % | 42.0 | 0.9733 | 0.0216 |
| 06_World | 4K | τ 0.05, 4×4 (**Low**) | 6.55 (-12 %) | 0.13 | 1.06 (-0.89) | 74 % | 39.1 | 0.9479 | 0.0315 |
| 06_World | 4K | τ 0.08, 2×2 (old High) | 6.62 (-11 %) | 0.13 | 1.13 (-0.82) | 81 % | 40.3 | 0.9672 | 0.0260 |
| 06_World | 4K | Motion, 4×4 | 7.57 (+2 %) | 0.13 | 1.94 (+0.00) | 0 % | 90.5 | 1.0000 | 0.0000 |
| 06_World | 4K | Foveated, 4×4 | 6.84 (-8 %) | 0.13 | 1.30 (-0.65) | 54 % | 37.7 | 0.9637 | 0.0253 |
| 06_World | 4K | pan: Off | 8.12 | — | 1.83 | — | — | — | — |
| 06_World | 4K | pan: τ 0.03, 2×2 | 7.30 (-10 %) | 0.13 | 1.04 (-0.79) | 86 % | 44.6 | 0.9939 | 0.0291 |
| 06_World | 4K | pan: Motion, 4×4 | 7.27 (-11 %) | 0.13 | 1.01 (-0.82) | 81 % | 39.3 | 0.9894 | 0.0344 |

VRS passes per frame: `VRS.Generate` 0.01 ms (1080p) / 0.03 ms (4K), `VRS.Analyze` 0.03 / 0.10 ms, plus
`VRS.AnalyzeOpaque` 0.03 / 0.10 ms in frames with refraction (02 glass, 05 water). Before the subgroup reduction one
analysis cost 0.42 ms at 4K.

Conclusions: Medium −2…−10 % at 1080p is mostly noise-level (the forward passes of the showcase cost 0.2–0.7 ms at
1080p on this GPU), 4K −6…−9 % except the glass station (±0: the second analysis eats the gain). Quality at Medium:
PSNR 44.6–51.8 dB, FLIP 0.007–0.016; Low (4×4) up to −12 % at 4K with PSNR 39–46 dB and visibly softer textures.
The first defaults (on at High with τ 0.08) were barely faster than τ 0.05 and lost texture detail (02 1080p PSNR
37.8 dB, FLIP 0.025). `Motion` mode: nothing on a static camera, as much as Adaptive τ 0.03 when panning.
`Foveated`: −2…−13 % but PSNR 33–46 dB (for HMDs).

Design and the rate image: `modules/render_vrs.md`; scalability values: `docs/guide/26-quality-settings.md`.

## Neural rendering (RTX 3080, 2026-10-14)

Validation off, RelWithDebInfo, driver 617.42, GPU lock held. Reproduce:
`OX_NTC_PERF=1 OX_VULKAN_VALIDATION=0 ox_render_gpu_tests --gtest_filter=NeuralTexturesTest.PerfReportMaterialStation`
(trains the sets once, cached in `%TEMP%/oxwald_ntc_cache`; `OX_NTC_PERF_PATHS=FP16,Auto` picks the material shader
paths) and `OX_NEURAL_PERF=1 OX_VULKAN_VALIDATION=0 ox_render_gpu_tests
--gtest_filter=NeuralGpuTest.InferenceThroughput:NeuralDenoiserTest.ComparedToSvgf`.

### Neural texture compression — size and quality (`PerfReportMaterialStation`, preset Medium)

Six materials of a station like showcase station 02 (generated 512² textures + a 2048² leather from the legacy 4K
set). "BC" = the regular importer output (BC7 sRGB albedo / BC7 ORM / BC5 normal, full mip chains, `.oxtex` bytes);
"NTC" = the `.oxntc` payload (latents + FP16 network + BC fallback tail ≤ 128 px), which is also the InferenceOnSample
VRAM. PSNR / SSIM against the source images; the BC column is the BC encoding of the same sources.

| material | size | BC bytes | NTC bytes | bits/texel | train s | PSNR albedo NTC / BC | normal NTC / BC | roughness NTC / BC | SSIM albedo NTC / BC |
|---|---|---|---|---|---|---|---|---|---|
| tiles | 512² | 1 048 656 | 194 960 | 3.95 | 5.3 | 45.7 / 52.7 | 39.0 / 52.7 | 42.2 / 53.8 | 0.986 / 0.997 |
| concrete | 512² | 699 104 | 173 088 | 3.95 | 5.1 | 44.4 / 54.1 | 37.5 / 52.3 | — | 0.977 / 0.998 |
| wood | 512² | 699 104 | 173 088 | 3.95 | 7.2 | 47.0 / 51.2 | — | 37.6 / 54.8 | 0.993 / 0.997 |
| leather | 2048² | 16 777 296 | 2 029 968 | 3.75 | 340 | 42.1 / 48.5 | 35.8 / 49.2 | 46.3 / 55.2 | 0.971 / 0.993 |
| rock | 512² | 699 104 | 173 088 | 3.95 | 13.7 | 48.4 / 52.9 | 38.1 / 52.3 | — | 0.988 / 0.996 |
| screen | 512² | 699 104 | 173 088 | 3.95 | 10.8 | 43.9 / 57.3 | — | — | 0.977 / 1.000 |
| **total** | | **20 622 368** | **2 917 280** (7.1x smaller) | | | | | | |

Load time of DecodeOnLoad (CPU, `decodeToTextures`: network + BC7/BC5 encode at `r.NeuralTextures.DecodeQuality 0`),
three runs while other processes loaded the CPU: 512² materials 118–716 ms (typically 120–180 ms: ~70 ms network +
~70 ms BC), leather 2048² 2.8–3.6 s (network 1.0–1.2 s + BC7 1.7–2.4 s). GPU decode of all neural mips of the leather
set (`NtcGpuDecoder`, no BC encode): CoopVector 1.2–1.3 ms, CoopMatrix 6.2–6.5 ms, FP16 10.3 ms, FP32 19–25 ms.

### Neural texture compression — GPU time (`PerfReportMaterialStation`)

Floor (tiles 4x4), wall (concrete), 18 objects; TAA, stochastic filtering unless noted; 24 frames per configuration,
the timing of the last one. "textures + latents" = `GpuResourceCache::textureBytes()` + latents + weights (the
texture streamer keeps only the wanted mips resident, hence less than the disk sizes).

| r.NeuralTextures | material shader path | 1080p frame ms | 1080p prepass + forward ms | 4K frame ms | 4K prepass + forward ms | textures + latents 1080p / 4K |
|---|---|---|---|---|---|---|
| Off (BC7 / BC5) | — | 0.89 | 0.26 | 3.38 | 0.94 | 6.8 / 9.8 MiB |
| DecodeOnLoad | — | 0.84 | 0.24 | 3.08 | 0.94 | 7.0 / 10.0 MiB |
| InferenceOnSample | Auto (CoopVector) | 3.12 | 1.91 | 11.62 | 7.24 | 3.0 / 4.7 MiB |
| InferenceOnSample, Bilinear | Auto (CoopVector) | 7.69 | 5.07 | 30.62 | 20.17 | 3.0 / 4.7 MiB |
| InferenceOnSample | FP16 | 14.13 | 9.36 | 55.24 | 36.72 | 3.0 / 4.7 MiB |
| InferenceOnSample | Scalar (FP32) | 27.22 | 22.74 | 100.31 | 65.13 | 3.0 / 4.7 MiB |

InferenceOnSample costs +1.7 ms at 1080p and +6.3 ms at 4K on this station (nearly every pixel is a neural
material) and halves texture memory; without cooperative vectors it is not usable (use DecodeOnLoad).

Before this measurement the material shaders always contained the decoder: with the cooperative-vector variant the
Off / DecodeOnLoad rows were 2.66 ms of prepass + forward at 4K (forward 2.26 ms) instead of 0.94 ms — on every
NVIDIA GPU with cooperative vectors, for every material, with neural textures not even in use. The renderer now keeps
a second pipeline set with the decoder (`prepassNeural` / `forwardNeural`), created on the first InferenceOnSample
frame (`modules/render_neural.md`; the meshlet path got the same split on 2026-10-15, see
"Neural texture decoder off the meshlet path" above).

### MLP paths — `NeuralGpuTest.InferenceThroughput`

Batch inference, 32 inputs → H → H → 16 outputs, FP32 in/out buffers, average of 8 timed dispatches:

| samples | hidden | ScalarF32 | ScalarF16 | CoopMatrix | CoopVector |
|---|---|---|---|---|---|
| 2.07 M (1080p) | 32 | 2.22 ms | 2.01 ms | 1.54 ms | 0.97 ms |
| 8.29 M (4K) | 32 | 21.6 ms | 8.19 ms | 5.84 ms | 3.45 ms |
| 2.07 M (1080p) | 64 | 7.38 ms | 4.56 ms | 2.20 ms | 1.21 ms |
| 8.29 M (4K) | 64 | 30.2 ms | 18.1 ms | 8.79 ms | 4.67 ms |

The KHR cooperative-matrix kernel was 65.3 ms (8.3 M, hidden 64) before tuning: shared-memory rows of 64 halves put
all 16 rows of a tile in the same bank (16-way conflicts on every `coopMatLoad`/`coopMatStore`) and the weights were
staged per 256-sample workgroup (44 KB shared memory per workgroup → low occupancy). Padding the rows by 8 halves
(65 → 29 ms), loading the weight tiles straight from the buffer (L1/L2) and 1024 samples per workgroup (→ 8.8 ms).
It stays ~2x slower than the driver's cooperative-vector kernels, so `bestComputePath` now prefers CoopVector >
CoopMatrix > FP16 > FP32.

### Neural denoiser — `NeuralDenoiserTest.ComparedToSvgf`

1-spp stochastic AO (`denoise_test_noise.comp`, no RT needed) through `rt::SvgfDenoiser`, RMSE / PSNR against a
256-spp reference, 192², then the cost at 1080p (same temporal and variance passes for both kernels):

| | frame 1 (fresh history) | frame 25 (converged) | 1080p denoiser | of which À-trous (4 iterations) |
|---|---|---|---|---|
| noisy input | 0.2214 (13.1 dB) | — | — | — |
| SVGF | 0.0476 (26.4 dB) | **0.0329 (29.6 dB)** | **2.49 ms** | **2.28 ms** |
| Neural | **0.0355 (29.0 dB)** | 0.0457 (26.8 dB) | 6.91 ms | 6.70 ms |

Training (`NeuralDenoiserTest.TrainWeights`, ~30 s): 22 frames (path tracer 1/2/4/16 spp vs 1536 spp, AO 1/8 spp vs
256 spp); held-out path traced frames on the CPU replica (spatial only): noisy 31.9 dB, SVGF 40.1 dB, neural 40.5 dB.
The À-trous shader first ran at 35.7 ms: a runtime iteration index into the 1604-float weight array put it in local
memory; one generated function per iteration (constant indices → immediates) gives 7.2 → 6.7 ms. The neural kernel
is better than SVGF right after a history reset but blurrier once the history has converged and ~3x the À-trous cost:
`r.Denoiser Neural` is marked experimental and not offered in the settings menus.

## Frame pacing: editor viewport and windowed player (RTX 3080, 2026-10-14)

User report: "40 fps in the editor on the RTX 3080, the same with ray tracing on and off; the status bar shows a frame
time below 1 ms". Machine as above; primary display 5120×1440 at **100 Hz**, second display 2560×1440 at 144 Hz.
RelWithDebInfo, validation on (the default outside Release), other builds running in parallel. Showcase project
(751 files under `Assets/`), default editor scene (Hub).

**How to measure.** Editor: `OX_EDITOR_VIEWPORT_STATS=1 OxwaldEditor --project samples/OxwaldShowcase
--smoke-seconds 12` — the viewport redraws continuously and logs per-second averages (fps, wall frame interval,
frame-slot wait, acquire, CPU, present, GPU from timestamps) plus the 8 most expensive passes. Player:
`OxwaldPlayer --stats ...` (or `OX_FRAME_STATS=1`) prints the same breakdown every second and a total at exit
(`IRenderer::stats()` now carries `lastFrameWaitMs / lastAcquireMs / lastCpuRenderMs / lastPresentMs /
lastGpuFrameMs / vsync`). The status bar and the viewport stats overlay show `FPS · frame interval · CPU · GPU`.

### Cause: the hot-reload file watcher walked the asset tree on the UI thread every engine tick

The editor ticks its `Engine` in edit mode from a 16 ms timer on the UI thread (`PlaySession`). With
`cfg.editor` / `fileWatching` the assets module called `AssetRegistry::poll()` every frame, and the registry's
`FileWatcher` (never `start()`ed) scanned `Assets/` synchronously: per file `is_regular_file` from the listing plus
**two extra stat calls** (`file_size`, `last_write_time`), ~25 µs each on NTFS with Defender. 751 files = **44–62 ms
per engine tick**. Found with a timing `QApplication::notify` override: every slow event was the 16 ms QTimer.
The viewport timer (8 ms) starved behind it: 13–15 viewport frames/s, while the viewport's own frame (what the
status bar showed) took 0.8–1.0 ms CPU and the GPU 5.6 ms — so ray tracing could not change the frame rate. On a
less loaded machine / Release the same cost gives the reported ~40 fps.

Fixes:
- `FileWatcher::scan` takes size and mtime from the cached `directory_entry` (no syscalls per file on Windows).
- `AssetRegistry::Options::watchScanInterval` (new, default 0 = old behaviour): the runtime sets 250 ms, the
  watcher scans on its own thread and `poll()` only delivers queued changes. The engine's own `FileWatcher` service
  is started in background mode too when `fileWatching` is on.
- rhi shader hot-reload poller: one `directory_entry` query per shader instead of two stat calls (runs every 200 ms
  on the render / UI thread, 186 shader files).

| editor viewport, Hub, default layout (4728×918), frame limit 120 | fps | frame interval | CPU ms | GPU ms |
| --- | --- | --- | --- | --- |
| before | 13–15 | 61–86 ms (engine tick 44–62 ms) | 0.8–1.0 | 5.6* |
| after, ray tracing off | **112–117** (old limit 125, see below) | 8.6–8.9 ms | 0.9–1.2 | 5.6 |
| after, `r.RayTracing 1` (1208×509 layout) | 109 | 9.2 ms | 1.1 | 4.3 |

\* first "before" runs logged 18–22 ms GPU while other agents' GPU tests were running; the GPU is not the limit in
either case. `ViewportPacingTests` (3000 extra files): 2.8 viewport ticks/s with the old watcher, ≥ 60 required.

### Viewport pacing to the display refresh rate (2026-10-15)

The viewport paces to the refresh rate of the screen it is on (`matchDisplayRefresh`, on by default):
`ViewportPanel::pacingFps` takes `QScreen::refreshRate()` of the viewport's current screen (Qt 6.10 reports the exact
mode rate, e.g. 59.94 / 143.98), and `FramePacer` turns it into millisecond single-shot timer delays whose average
equals the target exactly. 144/165/240 Hz monitors are no longer capped at the old 125 fps (8 ms timer for the 120
limit); moving the window to another monitor restarts the schedule at that screen's rate. `frameLimit` (default 120,
0 = unlimited) applies only with the option off; unfocused windows throttle to `unfocusedFps` (10). Measured in
tests: 120.4 ticks/s for a 120 limit, 60.0 ticks/s on a 60 Hz screen.

Release preset, Hub, default layout (4728×918), 100 Hz monitor, continuous redraw (`OX_EDITOR_VIEWPORT_STATS=1`),
30 s per run, 2026-10-15:

| editor viewport | display rate (default) | frame limit 0 (uncapped) |
| --- | --- | --- |
| first measurement | 91–94 fps | 138–143 fps |
| `FramePacer` catches up a tick up to 2 periods late | 92.5–93.6 fps | 142–144 fps |
| + `StatsPanel` updates its rows in place | **99.3–100.7 fps** (CPU 0.68 ms, GPU 6.5 ms) | **138–153.5 fps** (GPU-bound) |

The remaining loss was a 30–45 ms UI-thread stall every 500 ms: `StatsPanel::refresh` re-created ~75 rows of labels
and progress bars (~700 events per refresh), longer than the pacer's catch-up window. Test
`ViewportPacingTests::statsTableUpdatesRowsInPlace`. In normal use (no continuous redraw) the viewport redraws when
the 16 ms edit tick marks it dirty, so the status bar reads ~60 fps whatever the display rate.

### Windowed player — not CPU or sync bound

`OxwaldPlayer --project samples/OxwaldShowcase --scene project://Assets/Scenes/Hub.oxscene --frames 600 --width
1920 --height 1080 --stats`, averages after the first second:

| configuration | fps | frame ms | render thread: slot wait / CPU / present | GPU ms |
| --- | --- | --- | --- | --- |
| raster, vsync (FIFO, 100 Hz display) | 99.3 | 10.07 | 8.70 / 1.23 / 0.13 | 3.39 |
| raster, `r.VSync 0` (mailbox) | 289 | 3.46 | 1.46 / 1.57 / 0.39 | 3.17 |
| raster, vsync, validation off | 100.0 | 10.00 | 8.92 / 0.97 / 0.11 | 3.09 |
| `r.RayTracing 1`, vsync | 81 | 12.30 | 9.82 / 2.14 / 0.24 | 11.17 |
| `r.RayTracing 1`, `r.VSync 0` | 89.5 | 11.17 | 9.81 / 1.14 / 0.21 | 11.12 |

The player reaches the display refresh (vsync on) or the GPU limit (ray tracing, 11 ms GPU at 1080p); the CPU
renderer stays at 1–2 ms, validation costs < 0.3 ms here. No per-frame `vkDeviceWaitIdle` in the frame loop (only
resize, view destruction, screenshots, shutdown).

### Present path (for the Reflex / frame generation work)

Player: render thread, `Device::beginFrame()` (waits on the timeline values of the frame slot `frameIndex()`,
2 frames in flight) → `Swapchain::acquire()` (recreates on size/present-mode change, `vkAcquireNextImageKHR` with
per-frame-slot acquire semaphores) → `Renderer::beginFrame/renderView/endFrame` (the graph submits with the
acquire wait / present signal semaphores) → `Swapchain::present()` (per-image present semaphores, graphics queue
mutex) → `Device::endFrame()`. Present mode: `r.VSync` 1 = FIFO, 0 = mailbox (immediate only when requested).
The game thread runs at most one frame ahead (two snapshot slots); with FIFO the back-pressure shows up as "slot
wait" on the render thread and as "waitRender" on the game thread. Editor: the same sequence on the Qt UI thread in
`VulkanViewportWindow::renderFrame` (mailbox swapchain, single command list recorded by the renderer), paced by the
viewport timer (display refresh rate by default, `frameLimit` otherwise, see
[above](#viewport-pacing-to-the-display-refresh-rate-2026-10-15)). A latency marker / frame generation hook naturally
goes around `acquire()` (sim/render start) and `present()`. The Reflex and FSR 3 frame generation results are in
[Latency](#latency-nvidia-reflex-engine-limiter-fsr-3-frame-generation-rtx-3080-2026-10-15).

## Latency: NVIDIA Reflex, engine limiter, FSR 3 frame generation (RTX 3080, 2026-10-15)

Windowed `OxwaldPlayer` (RelWithDebInfo, validation off) on the Showcase, RTX 3080 (driver 617.42), i9-9900K, monitor
5120×1440 at 100 Hz. Window `--width 2560 --height 1440` (client area / swapchain 2560×1421). Each row is 3 runs with
a fresh user dir. Per run: the median of the 1-second `--stats` windows, skipping the first three (loading,
pipeline compiles). The table shows the median of the 3 runs; the spread between runs was ≤ 3 % unless noted.
**Latency here is not end-to-end (click to photon).** There is no hardware latency analyzer. Two measures are used:
- *Engine latency* (`LatencyStats::engineLatencyMs`): simulation start → GPU finished the frame. CPU timestamps: the
  game thread's `SimulationStart` marker and a thread waiting on the graphics timeline. Works for any vendor and also
  through the frame generation proxy.
- *PC latency*: NVIDIA's simulation start → GPU render end, from `vkGetLatencyTimingsNV` (`VK_NV_low_latency2`). It
  is reported only while Reflex owns the swapchain. Both measures agree within 0.2 ms wherever both exist.

Neither measure includes scan-out or display time. With frame generation, the rendered frame is also held by the
proxy for *hold* = `RenderStats::frameGeneration.addedLatencyMs`: from our present call until the proxy composes the
frame for its own present, after the generated frame in front of it. Fallback-limiter rows use `OX_DISABLE_REFLEX=1`
(device created without `VK_NV_low_latency2`, so `r.Reflex On` runs the engine limiter on NVIDIA hardware).

GPU bound — station 4 (Volumetrics), `--quality ultra`, `r.VSync 0` (mailbox):

| Mode | fps | GPU ms | engine latency ms | PC latency ms | latency sleep ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| `r.Reflex Off` | 104.1 | 9.53 | 46.9 | 46.9 | 0 |
| `r.Reflex On` | 101.2 | 9.50 | 14.0 | 14.0 | 9.4 |
| `r.Reflex OnBoost` | 102.0 | 9.42 | 13.8 | 13.7 | 9.3 |
| limiter `JustInTime` (no Reflex) | 82.8 | 9.13 | 22.6 | — | 11.6 |
| limiter `Strict` (no Reflex) | 91.7 | 9.28 | 10.9 | — | 10.4 |

CPU bound — station 7 (Physics), `--quality low`, 960×540 window, `r.VSync 0`:

| Mode | fps | GPU ms | engine latency ms | PC latency ms |
| --- | ---: | ---: | ---: | ---: |
| `r.Reflex Off` | 1516 | 0.31 | 2.05 | 2.01 |
| `r.Reflex On` | 1511 | 0.31 | 2.04 | 2.00 |
| `r.Reflex OnBoost` | 1521 | 0.31 | 2.03 | 1.99 |
| limiter `JustInTime` (no Reflex) | 1524 | 0.31 | 1.44 | — |
| limiter `Strict` (no Reflex) | 832 | 0.34 | 1.21 | — |

FSR 3 frame generation (`r.FrameGeneration FSR3`). Station 4 as above. Reflex cannot run through the proxy, so with
`r.Reflex On` the engine limiter runs:

| Mode | rendered fps | presented fps | ratio | GPU ms | engine latency ms | hold ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| frame generation off, `r.Reflex Off` (from the first table) | 104.1 | 104.1 | 1 | 9.53 | 46.9 | — |
| frame generation off, `r.Reflex On` | 101.2 | 101.2 | 1 | 9.50 | 14.0 | — |
| FSR 3, `r.Reflex Off` | 81.5 | 162.7 | 2.00 | 10.25 | 45.8 | 24.0 (22.7–28.4) |
| FSR 3, `r.Reflex On` → limiter `JustInTime` | 73.8 | 147.1 | 2.00 | 9.93 | 21.4 | 23.2 |
| FSR 3, `r.Reflex On`, `r.Reflex.Fallback Strict` | 79.6 | 158.7 | 2.00 | 10.33 | 12.5 | 13.3 |

Vsync (FIFO, 100 Hz) — station 1 (Lighting), quality High (GPU 4.2 ms):

| Mode | fps (presented) | engine latency ms | PC latency ms | hold ms |
| --- | ---: | ---: | ---: | ---: |
| `r.Reflex Off` | 100.0 | 48.6 | 48.4 | — |
| `r.Reflex On` | 100.0 | 46.5 | 46.4 | — |
| `r.Reflex Off`, `t.MaxFPS 97` | 97.0 | 6.4 | 6.3 | — |
| `r.Reflex On`, `t.MaxFPS 97` | 97.0 | 6.4 | 6.2 | — |
| FSR 3, `r.Reflex Off` | 50 / 99.9 | 47.3 | — | 20.8 |
| FSR 3, `r.Reflex On` (JustInTime) | 50 / 99.9 | 25.2 | — | 20.8 |
| FSR 3, `r.Reflex On`, `t.MaxFPS 49` | 49 / 97.8 | 7.9 | — | 9.0 |

Reading:
- **Reflex**, GPU bound: engine latency 46.9 → 14.0 ms (−70 %) for −3 % fps. The ~3.5 frames queued behind the GPU
  shrink to about one. Boost changes nothing measurable on a desktop RTX 3080, which does not downclock under this
  load. CPU bound: nothing to remove (2 ms, no queue).
- **Engine limiter**, used without Reflex (AMD/Intel/Apple/Android and through the FSR 3 proxy). *Strict* beats
  *JustInTime* when GPU bound: 10.9 ms at 92 fps vs 22.6 ms at 83 fps. JustInTime oversleeps there: the GPU idles
  ~3 ms per frame, because Windows timer wake-ups and the per-frame CPU floor exceed its 1 ms headroom. When CPU bound,
  JustInTime is free (1.44 ms at full fps) and Strict halves the frame rate. JustInTime stays the default; tightening
  it is an open item (`docs/dev/modules/runtime.md` "Latency").
- **Vsync** without a frame cap: the queue sits in the presentation engine, and neither Reflex nor anything before
  the GPU removes it (48.6 → 46.5 ms). A cap just under the refresh rate (`t.MaxFPS 97` at 100 Hz) takes latency to
  6.4 ms with or without Reflex.
- **Frame generation** doubles the presented rate exactly (ratio 2.00 in every run). The rendered rate drops 104 →
  82 fps: our frame gets +0.7 ms GPU for the HUD-less copy and the prepare dispatch, and the rest is the proxy's
  interpolation and pacing on the same GPU. It adds the proxy hold: 24 ms here, about two rendered frames. The
  limiter is essential with it. JustInTime gives 45.8 → 21.4 ms engine latency for −9 % rendered fps. Strict gives
  12.5 ms and also shortens the hold to 13 ms, for −2 %. Rough simulation-to-composition totals: 70 ms (FSR 3 alone),
  45 ms (+ JustInTime), 26 ms (+ Strict), against 14 ms for Reflex without frame generation.

Reproduce (Git Bash; set the validation layers off). Run in `E:/Development/OxwaldEngine/build/<dir>/bin`:

    OX_VULKAN_VALIDATION=0 ./OxwaldPlayer.exe --project <repo>/samples/OxwaldShowcase \
      --scene project://Assets/Scenes/Stations/04_Volumetrics.oxscene --windowed --width 2560 --height 1440 \
      --quality ultra --stats --frames 1500 --user-dir <empty dir> --cvar r.VSync=0 --cvar r.Reflex=On
    # limiter rows: prefix OX_DISABLE_REFLEX=1, add --cvar r.Reflex.Fallback=JustInTime|Strict
    # frame generation: add --cvar r.FrameGeneration=FSR3 (needs amd_fidelityfx_vk.dll next to the exe)
    # CPU bound: --scene .../07_Physics.oxscene --width 960 --height 540 --quality low --frames 8000
    # vsync: --scene .../01_Lighting.oxscene --cvar r.VSync=1 [--cvar t.MaxFPS=97]

Use a fresh `--user-dir` per run. With a reused one, the first run of a new configuration once ran at the previous
configuration's quality: station 4 Ultra showed 2.97 ms GPU instead of 9.5 ms. That looks like saved user settings
winning over `--quality`. The runtime owner should check this.
