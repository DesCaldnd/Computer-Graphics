# VRS handoff (2026-10-14, second session — area finished)

## State
- Feature, RHI support, tests and docs complete; details in `docs/dev/modules/render_vrs.md` (no provisional values).
- Coordinator merge material (perf table, CHANGELOG, guide README line): `docs/dev/handoff/out-vrs.md`.
- Build dir `E:/Development/OxwaldEngine/build/vrs` (clean rebuild from bash 2026-10-14 23:50; `#deps 0` = 0).

## What changed this session
- Root cause of the golden regressions (ManyPointLightsClustered, SsrMirrorFloor, GtaoCorner,
  RefractiveSphereAbsorption, TerrainSplatLayersAndCsm, TextureStreamingRespectsBudget,
  AsyncPipelineCompileUsesPlaceholderUntilReady): r.VRS defaulted to on (τ 0.08), and `TranslucencyTest` leaks
  Shading = High into later tests. Now `r.VRS` default off; presets on only at Low / Medium; thresholds
  0.05 / 0.03 / 0.02 / 0.02, max rate 4 / 2 / 2 / 2.
- Real bugs fixed: coarse-tile estimator (÷s → ÷√s), texture-LOD lock-in (`r.VRS.RefreshPeriod` 8), refraction
  (`VRS.AnalyzeOpaque` combined only with refraction), analysis cost 0.42 → 0.10 ms at 4K (subgroupAdd).
- No per-frame allocations of the feature's own any more (pass list parsed only when `r.VRS.Passes` changes, attach
  returns a count instead of a vector of names); its render-graph pass declarations allocate like every feature's.
- `GpuDrivenTest.SteadyStateExtractAndDrawListsDoNotAllocate` is NOT VRS: `extractDestruction`
  (`engine/render/src/features/destruction/destruction_bridge.cpp`) builds a `std::unordered_set` per extract call
  (MSVC allocates its sentinel + buckets even when empty) — destruction area.
- An RHI "VRS pipeline variant" experiment was reverted: the pipeline flag alone does not change images
  (`RenderTest.VrsOffMatchesDeviceWithoutTheExtension`).

## Test results (build/vrs, clean rebuild, 2026-10-15)
- `ox_render_tests` 86/86; `ox_render_gpu_tests` 171 run: 166 pass, 4 skipped (opt-in / no-support), 1 fail =
  `GpuDrivenTest.SteadyStateExtractAndDrawListsDoNotAllocate` (destruction, see above). All VRS tests pass.
- Station measurements 1080p + 4K: `E:/Development/OxwaldEngine/vrs_perf/results_v4.csv`, tables in out-vrs.md.

## Open
- Not measured on a Low / Medium class GPU (where the presets turn it on). MoltenVK / Android untested.
- Test fixtures that set scalability groups do not restore them (TranslucencyTest) — shared test infrastructure.
