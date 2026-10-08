# Performance log

Rule from the roadmap: every optimisation starts with a measurement and ends with one. GPU times come from the render
graph's per-pass timestamps (`RenderStats::passes`, `gpuFrameMs` = sum of top-level passes), CPU times from
`RenderStats::cpuRenderMs` / wall clock around `beginFrame … endFrame` (Tracy zones `OX_PROFILE_ZONE` exist on every
step for deeper captures). Machine: Apple M4 Pro, MoltenVK 1.4.2, RelWithDebInfo, validation on (numbers in Release
are lower on the CPU side). All tables are reproducible with the named tests in `ox_render_gpu_tests`.

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
