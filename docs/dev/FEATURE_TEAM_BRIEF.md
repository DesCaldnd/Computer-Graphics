# Brief for render feature teams (wave 3)

Several teams build render features **in parallel** on top of the finished renderer core. Read this, then
`docs/dev/ARCHITECTURE.md`, `docs/dev/modules/render.md` (feature API, injection points, resource contracts, shader
conventions, golden tests), `docs/dev/modules/rhi.md` (render graph, bindless, DeviceCaps), and `core.md` (cvars,
scalability).

## Ownership (stay inside yours)
| Area | Code | Shaders |
| --- | --- | --- |
| reflections-ao | `engine/render/src/features/reflections/` | `engine/shaders/render/reflections/` |
| volumetrics | `engine/render/src/features/volumetrics/` | `engine/shaders/render/volumetrics/` |
| translucency-water-particles | `engine/render/src/features/translucency/` | `engine/shaders/render/translucency/` |
| raytracing | `engine/render/src/features/raytracing/` | `engine/shaders/render/raytracing/` |
| postprocess-upscalers | `engine/render/src/features/postprocess/` (+ `upscalers/`) | `engine/shaders/render/postprocess/` |
| gpu-driven | `engine/render/src/features/gpu_driven/` (+ the core draw path, see its brief) | `engine/shaders/render/gpu_driven/` |
| world-skinning | `engine/render/src/features/world/` | `engine/shaders/render/world/` |
| ui | `engine/ui/` (new module) | `engine/shaders/ui/` |

- Public headers you add go to `engine/render/include/oxwald/render/features/<area>/` (ui: `engine/ui/include/oxwald/ui/`).
- Tests: `engine/render/tests/<area>_*.cpp` (GPU tests labelled `gpu`), goldens in `engine/render/tests/data/golden/<area>_*.png`.
  **Look at every golden image you create** (Read tool) and only accept it when it is visually correct.
- Registration: in `engine/render/src/renderer.cpp`, `registerBuiltinFeatures()` contains one marker line per area
  (`// [feature-area: <area>]`). Replace **only your marker** with your registration call. Re-read the file right
  before editing (others edit their own lines concurrently) and use an exact single-line replacement.
- Shared files (`engine/shaders/render/common/*`, `render_feature.hpp`, `renderer.cpp` beyond your marker, rhi):
  only small **additive** changes when strictly necessary; keep existing signatures; list them in your report.
- New ECS components you need (e.g. `ReflectionProbeComponent`, `FogVolumeComponent`, `ParticleEmitterComponent`,
  `PostProcessVolumeComponent`): define them in `engine/render/include/oxwald/render/components/<area>.hpp`, reflect
  + register them in your area's `register...Types()` called from `render::registerRenderTypes()` (additive line) and
  extract them in your feature through the snapshot extension hooks described in render.md.
- CVars: register yours with Scalability groups and per-level tables (Low/Medium/High/Ultra). The overall quality
  presets must produce sensible results. Don't redeclare cvars owned by others (`r.RayTracing`, `r.Upscaler*` are
  owned by render core; runtime owns window/vsync/FOV cvars).

## Build & test
```sh
cmake --preset dev -B build/<area> -DVCPKG_MANIFEST_INSTALL=OFF \
      -DOX_MODULES="core;scene;rhi;assets;world;render"      # add modules you need (ui, animation, gameplay, ...)
cmake --build build/<area> --target ox_render_tests
ctest --test-dir build/<area> -L render --output-on-failure
```
- macOS arm64 / Apple M4 Pro / MoltenVK 1.4.2: **no ray tracing, no mesh shaders, no geometry shaders, no
  drawIndirectCount**; multiview + vertex `gl_Layer` available; BC formats available. Gate everything by `DeviceCaps`
  and provide fallbacks; GPU tests needing missing features must `GTEST_SKIP()`.
- **MoltenVK shadow-sampler bug**: depth-compare reads must use `OX_SAMPLE_SHADOW*` / `oxShadowTextures2D*` from
  `bindless.glsl`, never a shadow sampler on `oxTextures2D[]`.
- Validation errors fail tests. Keep the whole `ox_render_tests` suite green (other teams' tests included) — if a
  failure is caused by another team's in-progress work, re-run later rather than editing their code.
- No git state changes; don't edit vcpkg.json.

## Report (final answer, ≤ 500 words)
Features + cvars (with scalability tables), components added, golden images list, GPU timings on M4 Pro at 1080p,
what's untestable on this Mac and how it is gated, shared files touched, problems, TODOs.
