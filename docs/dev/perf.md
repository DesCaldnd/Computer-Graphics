# Performance log

Rule from the roadmap: every optimisation starts with a measurement and ends with one. GPU times come from the render
graph's per-pass timestamps (`RenderStats::passes`, `gpuFrameMs` = sum of top-level passes), CPU times from
`RenderStats::cpuRenderMs` / wall clock around `beginFrame … endFrame` (Tracy zones `OX_PROFILE_ZONE` exist on every
step for deeper captures). Machine: Apple M4 Pro, MoltenVK 1.4.2, RelWithDebInfo, validation on (numbers in Release
are lower on the CPU side). All tables are reproducible with the named tests in `ox_render_gpu_tests`. The numbers
measured on the Windows machine are in their own section, [NVIDIA RTX 3080](#nvidia-rtx-3080-2026-10); everything
else is the M4 Pro.

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
show the trend. Nothing here was re-measured on an idle machine, at 4K or in Release. The ray tracing table at the
end of the section is the exception: it was measured separately, with validation off, at 1080p and 4K.

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

Showcase stations at 4K (3840×2160, headless player, quality High, ray tracing off, validation off), GPU frame time
in ms:

| Mode | Station 4 (volumetrics) | Station 6 (world) |
| --- | --- | --- |
| Off | 37–39 | 24.7 |
| TAA native | 38–41 | 26.1 |
| FSR 1 Quality | 21.0–21.7 | unstable: 52.8 and 58.9 |
| TAAU Quality | 21.0 | unstable: 14.1 and 50.5 |
| DLSS Quality | 23.1–23.9 | 16.4 |
| DLSS Performance | 18.2–18.5 | 14.6 |

Station 4: three runs, range shown. Station 6: the value of the last frame's overlay; FSR 1 and TAAU give either
~14 ms or 50–59 ms with identical settings, cause unknown (see BACKLOG). Reproduce with `OxwaldPlayer --headless
--width 3840 --height 2160 --scene project://Assets/Scenes/Stations/<station>.oxscene --frames 300 --cvar
r.AntiAliasing=2 --cvar r.Upscaler=<0..3> --cvar r.Upscaler.Quality=<0..4> --screenshot out.png -- --photo`.

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

With ReSTIR: RT.ReSTIR.Initial 0.64 / 2.53 ms, RT.ReSTIR.Spatial 0.37 / 1.23 ms. Path tracer at 1 sample per frame:
RT.PathTrace 1.13 / 4.45 ms (mode 0), RT.PathTrace.Pipeline 1.46 / 5.84 ms (mode 1); the hybrid passes above keep
running under it (BACKLOG).

The four denoised signals take 5.9 of 9.2 ms at 1080p and 22.5 of 31.2 ms at 4K: the SVGF denoiser is the cost, the
rays are not. `DenoiserSettings::clampHistory` (shadow signals) added 0.06 ms per signal at 1080p. The DDGI probe
passes and the froxel visibility do not scale with the resolution.
