# Render performance tails — handoff (2026-10-15)

## State
- The three "Хвосты оптимизации" render items of `docs/dev/HANDOFF.md` are done (the editor 144 Hz timer item was
  not part of this task), plus the coordinator's extra item (NTC decoder off the meshlet path).
- Merge material for `perf.md` / `CHANGELOG.md`: `docs/dev/handoff/out-perf.md`.
- Build dir `E:/Development/OxwaldEngine/build/perf` (RelWithDebInfo), built only from bash (`ninja -t deps`:
  3 × `#deps 0`, the Qt rcc/moc objects). Python with numpy/pillow for PSNR: `E:/Development/OxwaldEngine/build/perf_venv`.
  Measurement outputs (CSV, screenshots, crops): `E:/Development/OxwaldEngine/build/perf_inv`.

## Changes
1. Path tracer: `rt::pathTracerActive()` (raytracing.hpp / rt_settings.cpp); `RtFeatureBase::gather(ctx, in,
   hybrid = true)` returns false for hybrid effects under it (rt_internal.hpp); the path tracer calls it with
   `hybrid = false`. Test `RayTracingTest.HybridEffectsSkipUnderPathTracer`.
2. Station 6 bimodality: no engine defect; contention of other GPU processes inside single-frame readings (evidence
   in out-perf.md). Tooling: `OxwaldPlayer --pass-stats` / `--pass-csv` (apps/player/main.cpp, forces
   `--single-thread` because `render::Renderer::stats()` is read on the game thread).
3. Station 4: volumetrics already resolution independent; the 4K cost was GTAO + SSR at full resolution. New
   `r.AO.HalfResAboveHeight` / `r.SSR.HalfResAboveHeight` (reflections/cvars.cpp, reflection_internal.hpp
   `cv::screenSpaceDownsample`, ao_feature.cpp, reflections_feature.cpp). Test
   `ReflectionsTest.ScreenSpaceTracesHalveAboveRenderHeight`. Docs: render_reflections_ao.md, guide 21, guide 26.
4. Meshlet / mesh-shader pipelines: two sets `[0 = OX_NTC_DISABLED, 1 = decoder]` (gpu_driven.cpp/.hpp,
   `GpuDriven::materialSet()`), neural set created on first InferenceOnSample use. Test
   `NeuralTexturesTest.MeshletPathDecodesNeuralMaterials`; `GpuDrivenTest.StressSceneTimings` gained
   `OX_RENDER_PERF_RES` and `OX_RENDER_PERF_CVARS`.
- Docs also touched: render.md (RT feature table note), guide 24 (path tracer rows), BACKLOG.md (three entries),
  reports/rtx-bringup-2026-10-14.md §7.

## Open
- Under the path tracer the raster frame (prepass, forward, volumetric fog, translucency, ~0.6 ms at 1080p on the RT
  test scene) still runs although SceneColorHDR is overwritten (BACKLOG).
- Final numbers are RelWithDebInfo (`build/perf`), not the Release preset; GPU-bound, so they should not differ.
- The 2026-10-14 station numbers (37–39 ms, 50–59 ms) could not be reproduced on the current tree; their exact
  origin (old code vs contention) cannot be separated after the fact.
- The editor viewport 144 Hz timer item of the same list is not done (not assigned).
