# Merge material — render performance tails (2026-10-15)

Everything measured on the RTX 3080 (driver 617.42), validation off, `build/perf` (RelWithDebInfo; GPU-bound
numbers, the build type does not change them), under the GPU lock (`E:\Development\OxwaldEngine\gpu-perf.lock`).
Showcase numbers: headless `OxwaldPlayer --quality high ... --frames 300 --pass-stats -- --photo`, median GPU frame
time over frames 61–300 (the new `--pass-stats` option, see below).

## For `docs/dev/perf.md`

### Replace the paragraph after the "Per pass" table of "Ray tracing — `RayTracingTest.PerfReportRayTracing`"

> With ReSTIR: RT.ReSTIR.Initial 0.64 / 2.53 ms, RT.ReSTIR.Spatial 0.37 / 1.23 ms. Path tracer at 1 sample per
> frame: RT.PathTrace 1.13 / 4.45 ms (mode 0), RT.PathTrace.Pipeline 1.46 / 5.84 ms (mode 1).
>
> **Path tracer no longer pays for the hybrid effects (2026-10-15).** The hybrid passes (shadows, AO, GI,
> reflections, volumetric visibility, refraction, their SVGF stages) kept running under `r.PathTracing` although the
> path traced image replaces SceneColorHDR. Now `RtFeatureBase::gather()` returns false for every hybrid effect while
> `rt::pathTracerActive()`: no pass, no target (test `RayTracingTest.HybridEffectsSkipUnderPathTracer`; 32 textures
> / 470 MB under the path tracer against 99 / 610 MB with the hybrid effects on its test scene).
>
> | path tracer | 1080p GPU frame before → after, ms | 4K GPU frame before → after, ms |
> |---|---|---|
> | mode 0 (ray query) | 10.28 → 1.67 | 36.06 → 5.63 |
> | mode 1 (RT pipeline) | 10.37 → 2.17 | 36.01 → 7.03 |
>
> "Before" is the `build/dev` binary of the same day (same test, same machine); RT passes under the path tracer
> dropped from 9.50 to 1.05 ms (1080p) and from 34.10 to 4.09 ms (4K): only RT.BuildAS + RT.PathTrace remain.

### Replace the station table and its paragraph in "DLSS pass"

> Showcase stations (headless player, quality High, ray tracing off, validation off), median GPU frame time in ms
> over frames 61–300 (`--pass-stats`), 2026-10-15:
>
> | Mode | Station 4 (volumetrics) 1080p | Station 4 4K | Station 6 (world) 1080p | Station 6 4K |
> | --- | --- | --- | --- | --- |
> | Off (native) | 3.57 | 8.42 (12.94 before `*.HalfResAboveHeight`) | 2.60 | 7.46 |
> | FSR 1 Quality | — | — | 1.64 | 5.21 |
> | TAAU Quality | — | — | 2.18 | 6.68 |
>
> The 2026-10-14 table (station 4 "37–39 ms", station 6 FSR 1 / TAAU "14 or 50–59 ms") is not reproducible on
> the current tree. Station 6 was the overlay's value of the *last* frame. Per-frame GPU times (`--pass-csv`) show
> what makes single frames 5–10× slower: one pass of the frame (any pass: `FSR1.EASU` 37.6 ms in a frame whose other
> passes were normal, `AO.Trace` 8.6 ms in another) absorbs a time slice of another process on the same GPU. The
> RTX 3080 is shared with the Android emulator (gfxstream, `qemu-system-x86_64-headless`), the other agents' GPU
> tests and the browser; Windows time-slices GPU contexts, and the engine's per-pass timestamps
> (`ALL_COMMANDS` before and after a pass) include the slice. With the GPU lock and no other GPU test running, FSR 1 /
> TAAU at 4K are stable (FSR 1: p10 4.95, median 5.21, p90 6.01 ms; TAAU: 6.26 / 6.68 / 7.67), threaded and
> `--single-thread` alike; the first second of a run (pipeline compiles, texture streaming) reads 30–40 ms in the
> `--stats` windows. Nothing in the upscalers re-creates resources or pipelines per frame (both upscalers are two
> fixed compute passes on graph-transient textures, histories persistent). Measure with the median of
> `--pass-stats`, not one frame. Reproduce: `OxwaldPlayer --project samples/OxwaldShowcase --headless --width 3840
> --height 2160 --quality high --scene project://Assets/Scenes/Stations/06_World.oxscene --frames 300 --cvar
> r.AntiAliasing=2 --cvar r.Upscaler=1 --cvar r.Upscaler.Quality=3 --pass-stats --pass-csv frames.csv -- --photo`.
>
> Station 4 at native 4K: the volumetrics are not what grows with the resolution. All `Volumetrics.*` passes take
> 0.65 ms at 1080p and 0.8–1.2 ms at 4K (froxel grid 160×90×64 independent of the output; clouds at 1/2 resolution
> with 1/16 checkerboard updates; the composite is one fullscreen pass, 0.13 ms at 4K). The 4K frame was
> screen-space AO (`AO.Trace` 2.34 ms) and SSR (`Reflections.SSRTrace` 2.11 + `SSRResolve` 1.11 ms), traced at full
> resolution on High / Ultra. New `r.AO.HalfResAboveHeight` / `r.SSR.HalfResAboveHeight` (default 1800) trace them
> at half resolution from a render height of 1800 px: at native 4K the trace keeps 1080p density, the bilateral
> upsample / resolve keep the image. Station 4, 4K, High: 12.94 → 8.42 ms (AO.Trace 2.34 → 0.71, SSRTrace 2.11 →
> 0.58, SSRResolve 1.11 → 0.49 ms); screenshot PSNR against full resolution 40.93 dB, SSIM 0.9932 (run-to-run noise
> of two full-resolution runs: 39.83 dB, SSIM 0.9978; the crops are visually identical). Upscaled 4K (render
> height 1440) and 1080p / 1440p are unchanged.

### New subsection after "Mesh shaders and `drawIndirectCount`" (NVIDIA RTX 3080)

> ### Neural texture decoder off the meshlet path — `GpuDrivenTest.StressSceneTimings`
>
> The meshlet pipelines (compute expansion and `VK_EXT_mesh_shader`) compiled `depth_prepass.frag` /
> `forward.frag` with the NTC decoder in, like the instanced path did before the neural area's fix. They now keep two
> sets like the renderer: `OX_NTC_DISABLED` normally, the decoder set created on the first `InferenceOnSample`
> frame. Stress scene (10 000 instances, GPU culling + HiZ + meshlets), `ForwardOpaque`, decoder compiled in
> (`OX_RENDER_PERF_CVARS=r.NeuralTextures=InferenceOnSample`, the pipelines the meshlet path used before for every
> material) → `OX_NTC_DISABLED` set (`r.NeuralTextures=Off`):
>
> | resolution | ForwardOpaque decoder in, ms | ForwardOpaque `OX_NTC_DISABLED`, ms | GPU frame, ms |
> |---|---|---|---|
> | 1080p | 0.88–0.95 | 0.46–0.51 | 2.76–2.89 → 2.27–2.41 |
> | 4K | 2.52 (4.28 in a contended run) | 0.83–0.90 | 5.07 → 3.17–3.52 |
>
> `OX_RENDER_PERF_RES=<w>x<h>` and `OX_RENDER_PERF_CVARS=name=value,...` are new options of the test.

## For `CHANGELOG.md` (Unreleased)

### Changed
- Ray tracing: the hybrid effects (shadows, AO, GI, reflections, volumetric visibility, refraction, Ray
  Reconstruction guides) skip themselves while the path tracer replaces the frame — no passes, no targets. Path tracer
  frame on the RT test scene: 10.3 → 1.7 ms at 1080p, 36.1 → 5.6 ms at 4K (RTX 3080).
- Screen-space AO and SSR trace at half resolution once the render height reaches `r.AO.HalfResAboveHeight` /
  `r.SSR.HalfResAboveHeight` (default 1800 px, 0 = never): native 4K costs what the effects cost at 1080p density.
  Showcase station 4 at 4K: 12.9 → 8.4 ms, PSNR 40.9 dB against full resolution.
- GPU-driven meshlet and mesh-shader pipelines are built without the neural texture decoder (`OX_NTC_DISABLED`)
  unless `r.NeuralTextures` is InferenceOnSample: ForwardOpaque on the meshlet path 2.5 → 0.85 ms at 4K.

### Added
- `OxwaldPlayer --pass-stats` / `--pass-csv <file>`: GPU time of every render graph pass averaged over the run and
  the GPU frame time distribution (min / p10 / median / p90 / max); implies `--single-thread`.

### Fixed (investigated)
- Showcase station 6 "bimodal" FSR 1 / TAAU frame time at 4K: not an engine defect — single-frame readings that
  included time slices of other processes on the shared GPU; stable 5.2 / 6.7 ms (median) on an idle GPU.
