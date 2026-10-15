# Neural rendering handoff (updated 2026-10-14, second session)

Build dir: E:/Development/OxwaldEngine/build/neural (vs.bat wrapper in scratchpad). Targets built: ox_render_gpu_tests,
ox_render_tests, ox_assets_tests, oximport, oxshowcase_generate. Editor not built here (only a string changed in
editor/src/settings/render_cvars.cpp). Text for perf.md / CHANGELOG: `docs/dev/handoff/out-neural.md`.

## State: done
- NTC CPU/GPU, importer, runtime modes, docs (guide 36 incl. step 6 numbers, render_neural.md).
- `NeuralTexturesTest.InferenceOnSampleMatchesRegularTextures` passes (33.5 / 33.7 dB). Root cause of the 25.7 dB
  failure was **r.VRS** (then on by default): content-adaptive VRS shaded the flat test plane in 2x2/4x4 blocks
  differently per mode; the streamer was fine (decoded textures fully resident). Test pins `r.VRS false`.
  With `r.VRS true` the numbers reproduce exactly (inference 30.28, decode 25.73) — info for the VRS area.
- `.oxmat` writes `neuralTextures` / `neuralTexturesOnly` only when set (engine/assets/src/material.cpp, test
  `Material.JsonWritesNeuralFieldsOnlyWhenSet`): `oxshowcase_generate --check` 169 → 2 differences.
- Denoiser trained (weights in engine/shaders/render/neural/denoiser_weights.glsl), shader 35.7 → 6.7 ms at 1080p
  (constant weight indices); quality: better than SVGF on frame 1, worse converged → marked EXPERIMENTAL (cvar
  description, editor cvar mirror, guide, module doc). Training PSNR metric fixed (physical units saturated x/(1+x)).
- Coop matrix kernel 65 → 8.8 ms (padding, weights from buffer, 1024 samples/WG); `bestComputePath` now CoopVector >
  CoopMatrix > FP16 > FP32 (test in NeuralGpuTest.CapsAndPaths).
- Renderer: second prepass/forward pipeline set with the NTC decoder, created on first InferenceOnSample frame;
  Off/DecodeOnLoad use `OX_NTC_DISABLED` (regular materials were 2.66 → 0.94 ms prepass+forward at 4K).
- Station benchmark test: neural screen material now gets the emissive factor (was black emissive);
  `OX_NTC_PERF_PATHS` env selects shader paths.

## Open
1. The 2 remaining `oxshowcase_generate --check` differences are not NTC:
   - `OxwaldShowcase.oxproj`: the generator output equals HEAD; the difference is the user's uncommitted edit
     (UI volume 0.9, empty rendering cvars instead of `r.AntiAliasing 2`, maps key order, new network/packaging/
     physics/scripting sections). Not regenerated (would overwrite the edit).
   - `Scenes/Stations/09_AI.oxscene`: real change, not noise — `Assets/AI/guard.oxbt.meta` got a new random UUID
     (a965593d-…, file rewritten 2026-10-14 17:40 by something in the session) instead of the committed
     f0eadad5-9e1d-8949-9966-c395d546d9a1; the committed scene references the old UUID three times (guard behavior
     trees). Fix: restore the uuid in guard.oxbt.meta (then the check is clean except the .oxproj), or regenerate.
2. gpu_driven.cpp meshlet pipelines compile forward.frag / depth_prepass.frag without `materialShaderDefines`
   (FP32 decoder always compiled in) — same slowdown as above may apply to the meshlet path.
3. Neural denoiser quality: train on frames captured after the temporal pass (GPU variance) instead of the CPU spatial
   replica.
4. `GpuDrivenTest.SteadyStateExtractAndDrawListsDoNotAllocate` fails ("serial extract 2 allocations") — identical in
   build/dev reference binaries, unrelated to the neural code paths.
