# render / reflections-ao (raster reflections, AO, baked GI)

Code: `engine/render/src/features/reflections/`, shaders `engine/shaders/render/reflections/`, public headers
`render/components/reflections.hpp`, `render/features/reflections/{reflections,reflection_gpu_types}.hpp`.
Registered by `registerReflectionFeatures()` (renderer.cpp marker); components + extract hook by
`registerReflectionTypes()` (also called from `render::registerRenderTypes()`).

## Features

| Feature (`r.Feature.<Name>`) | Group | Injection | Publishes |
| --- | --- | --- | --- |
| `AmbientOcclusion` | `AO` | Lighting (-100) | `AO` (R8) |
| `Reflections` | `Reflections` | Lighting (0), AfterOpaque | `ReflectionsSpecular`, `HiZClosest`, `PlanarReflection`, `SSR` |
| `IrradianceVolumes` | `IndirectDiffuse` | Lighting (100) | `IndirectDiffuse` |

Ray traced variants join the same groups with a higher priority.

* **Reflection probes** (`ReflectionProbeComponent`): one cube per probe (bindless, so every probe keeps its own
  resolution) captured with `SceneCapture` (passes/mesh.vert + `capture.frag`: material + `oxEvaluateLighting` for
  directional/IBL, local lights culled on the CPU per capture, cheap shadow filtering) and the sky; 2×2 mip chain;
  GGX prefilter per mip with `ibl/prefilter.comp`. Update modes: Baked (once / installed data / bake request),
  OnEnable (re-captured when re-activated or changed), Realtime (`r.ReflectionProbes.RealtimeFacesPerFrame` faces per
  frame). Visible probes are sorted (priority desc, then smaller box) and culled into clustered lists on the light
  cluster grid (`probe_cull.comp`, 16 per cluster). Per pixel: up to `MaxPerPixel` probes blended front to back by
  influence (1 inside the box, fading over `blendDistance` outside), parallax-corrected box projection, sky IBL for
  the remaining weight.
* **Planar reflections** (`PlanarReflectorComponent`, plane = entity XZ, normal +Y): mirrored camera
  (`reflectionMatrix`), the view's own projection made finite (`maxDistance`) with Lengyel's oblique near plane on the
  reflector (`obliqueReversedZ`, `clipOffset`), scissored to the reflector's screen footprint, clockwise front faces.
  Composited on opaque pixels lying on the plane (roughness ≤ `maxRoughness`).
* **SSR**: `HiZClosest` (max depth pyramid) → per-pixel GGX VNDF ray (mirror below roughness 0.05) → hierarchical
  march with thickness test → hit uv/pdf/confidence (edge, back-face, camera-facing fades) → spatial resolve reusing
  1/4/8 neighbour hits (BRDF/pdf ratio estimator, firefly-safe weights) fetching last frame's colour pyramid
  (reprojected with the hit's motion vector, mip = cone footprint) → temporal accumulation (neighbourhood clamp). The
  pyramid is written at AfterOpaque (SceneColorHDR / preExposure). Misses fall back to probes / sky.
* **AO**: GTAO (Jimenez 2016; Low uses hemisphere SSAO) at full/half res → 3×3 bilateral → temporal (depth-checked
  reprojection, clamp) → depth-aware upsample to `AO`. Lighting multiplies indirect diffuse by AO and derives the
  specular occlusion term from it (`oxSpecularOcclusion`). No bent normals.
* **Irradiance volumes** (`IrradianceVolumeComponent`): probe grid baked over frames
  (`r.GI.IrradianceVolumes.ProbesPerFrame`): 6 captures per probe → `volume_project.comp` (SH L1 + octahedral depth
  moments) → `indirect_diffuse.comp` (DDGI-style trilinear, backface + Chebyshev visibility, volume fade over the sky
  SH9 fallback).

## Contracts

* **Planar (water / mirrors)**: `VIEW.planarReflections` → `GpuPlanarReflections` (up to 4 reflectors: plane, bounds,
  distortion, max roughness, bindless texture). Textures are screen aligned (sample at the pixel's render uv, offset by
  the normal), RGBA16F radiance × preExposure. Helpers in `render/reflections/planar.glsl`
  (`oxFindPlanarReflection`, `oxSamplePlanarReflection` → not pre-exposed). The primary reflector is also the graph
  resource `PlanarReflection`: consumers declare a read on it.
* **IndirectDiffuse volume layout** (shared with DDGI): `GpuIrradianceVolumes` / `GpuIrradianceVolume` /
  `GpuIrradianceProbe` in `reflection_gpu_types.hpp`, mirrored by `render/reflections/irradiance_volume.glsl`
  (`oxSampleIrradianceVolume`). Probe = 4 × vec4 SH L1 (cosine-convolved / π, `sh[0].w` = valid); per-volume RGBA16F
  moment atlas, 10×10 tiles (8×8 octahedral + mirrored border), 64 tiles per row. A ray traced updater writes the same
  buffer/atlas and reuses the sampling code.
* **Probes**: `GpuReflectionProbe` + `render/reflections/probes.glsl` (influence, box projection, sampling).

## Baking API (editor "Bake probes")

`reflections::requestBake(renderer)` → keep rendering until `bakeInProgress()` is false →
`readBakedProbes()` / `readBakedVolumes()` (Uuid-keyed) → `saveOxCube` / `saveOxIrradiance` (render-owned binaries).
On scene load: `loadOx*` + `setBakedProbe` / `setBakedVolume` (Baked probes / volumes with that Uuid skip capturing).

## CVars (Low / Medium / High / Ultra)

| CVar | Default | Group levels |
| --- | --- | --- |
| `r.SSR` | false¹ | Reflections: off, on, on, on |
| `r.SSR.Quality` (reuse 1/4/4/8, temporal blend) | 2 | Reflections: 0, 1, 2, 3 |
| `r.SSR.MaxSteps` | 64 | Reflections: 24, 40, 64, 96 |
| `r.SSR.HalfRes` | false | Reflections: on, on, off, off |
| `r.SSR.MaxRoughness` | 0.7 | Reflections: 0.35, 0.5, 0.7, 0.85 |
| `r.SSR.Thickness`, `r.SSR.Temporal` | 0.3, true | — |
| `r.ReflectionProbes` | true | — |
| `r.ReflectionProbes.Resolution` | 128 | Reflections: 64, 128, 128, 256 |
| `r.ReflectionProbes.Realtime` | true | Reflections: off, on, on, on |
| `r.ReflectionProbes.RealtimeFacesPerFrame` | 1 | Reflections: 1, 1, 1, 2 |
| `r.ReflectionProbes.MaxPerPixel` | 4 | Reflections: 1, 2, 4, 4 |
| `r.ReflectionProbes.CapturesPerFrame` | 2 | — |
| `r.PlanarReflections` | true | Reflections: off, on, on, on |
| `r.PlanarReflections.ResolutionScale` | 0.75 | Reflections: 0.25, 0.5, 0.75, 1.0 |
| `r.PlanarReflections.MaxReflectors`, `r.Reflections.CaptureMaxLocalLights` | 2, 64 | — |
| `r.AO.Method` (0 off, 1 SSAO, 2 GTAO) | 0¹ | GlobalIllumination: 1, 2, 2, 2 |
| `r.AO.Quality` (GTAO slices×steps 1×4, 2×6, 2×8, 3×12) | 2 | GlobalIllumination: 0, 1, 2, 3 |
| `r.AO.HalfRes` | false | GlobalIllumination: on, on, off, off |
| `r.AO.Radius`, `r.AO.Intensity`, `r.AO.Temporal` | 0.75, 1, true | — |
| `r.GI.IrradianceVolumes` | true | GlobalIllumination: off, on, on, on |
| `r.GI.IrradianceVolumes.ProbesPerFrame` | 32 | — |

¹ Declared off so tools/tests that never choose a quality level render as before; every preset enables them.

## Tests

`tests/reflections_cpu_tests.cpp` (cube face matrices vs `oxCubeDirection`, mirror + oblique projection, `.oxcube` /
`.oxirr` round trips, components + extract, scalability tables) and `tests/gpu/reflections_tests.cpp`. Goldens:
`reflections_probe_spheres`, `reflections_ssr_probe`, `reflections_ssr_mirror_floor`, `reflections_box_projection`,
`reflections_planar_mirror`, `ao_gtao_corner`, `ao_gtao_corner_debug`, `gi_irradiance_bleeding`.

## Performance (Apple M4 Pro, 1080p, `ReflectionsTest.PerfReport1080p`, High values)

GTAO full res 1.3 ms (trace 0.75, denoise 0.3, temporal 0.18, upsample 0.1); SSR 2.0 ms (HiZClosest 0.2, trace 0.97,
resolve 0.39, temporal 0.23, colour pyramid 0.22); planar (12×12 m mirror, 0.75 scale) 0.6 ms; probe cull + composite
0.2 ms; IndirectDiffuse 0.5 ms. Probe captures cost one scene render per face (only when captured).

## Limits / TODO

* Captures (probes, volumes, planar) use the main view's cascades for the sun (receivers outside the cascade range
  are unshadowed) and no AO/SSR/probes inside the captured surfaces (sky IBL only, single bounce GI).
* SSR uses last frame's colour (one frame of lag; no translucents). No bent normals / cone-traced specular occlusion.
* Probe influence is a box; no sphere probes. Probe cubes are separate bindless cubes (no cube array).
