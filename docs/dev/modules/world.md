# Module `world`

CPU side of the open world: terrain heightfields (import, procedural generation, erosion, brushes, splat
maps, holes), CDLOD terrain LOD selection + grid mesh + physics tiles, vegetation scattering and culling,
sun/moon/stars astronomy, analytic sky, time of day with curves, wind and weather, Gerstner water with
buoyancy, chunk streaming and chunk serialization. Namespace `ox::world`.

Depends only on `core` (`types`, `log`, `assert`) + glm; stb (PNG) and tinyexr (EXR) privately. It is
**ECS-agnostic** and does **not** link physics — terrain and trees produce plain collider data, converted to
`ox::physics::ShapeDesc` by the header-only `oxwald/world/physics_bridge.hpp` (include it only where
`Oxwald::physics` is linked). GPU rendering is done by the renderer; this module provides the data, the
algorithms and GLSL includes that mirror the CPU math.

World convention: Y up, **north = −Z, east = +X**, azimuth measured from north clockwise.

| Header | Contents |
| --- | --- |
| `world.hpp` | umbrella include |
| `common.hpp` | `IRect` (dirty rects), `Aabb`, `Frustum` (from view-projection, reversed-Z/infinite safe), `DebugLineFn` |
| `noise.hpp` | `hash32`, `Rng` (PCG32), `Noise2D` (Perlin/Simplex), `FractalNoise` (fBm/ridged/billow + domain warp) |
| `heightfield.hpp` | `Heightfield` (f32 or u16 storage, normalized samples, sampling, holes, tiles, PNG16/R16/EXR import, PNG16/R16 export) |
| `terrain_gen.hpp` | `generateNoise`, `addNoise`, `erodeHydraulic` (droplets), `erodeThermal`, `terrace`, `computeStats` |
| `terrain_brush.hpp` | `applyBrush` (raise/lower/smooth/flatten/noise/holes, falloff) → dirty `IRect`; `paintSplat` |
| `splat_map.hpp` | `SplatMap` (≤ 8 layers, u8, two RGBA8 textures), `SplatRule`, `autoPaint` |
| `terrain_lod.hpp` | CDLOD: `TerrainQuadtree`, `LodRanges`, `TerrainPatch`/`TerrainPatchGpu`, morph helpers, `generateTerrainGrid`, `PhysicsHeightfieldTile`, `buildPhysicsTile(s)` |
| `vegetation.hpp` | `PoissonDisk`, `VegetationLayer`, `VegetationScatterer`, `VegetationInstance`/`VegetationInstanceGpu` (64 B), `VegetationCollider`, cells, `selectVegetationLod`, `cullVegetationCells` |
| `sky.hpp` | `julianDay`, `computeSunPosition` (NOAA), `computeMoonPosition`, `computeMoonPhase`, `starsRotation`, `sunLight`/`moonLight`, `PreethamSky` |
| `time_of_day.hpp` | `Curve`, `Gradient`, `AtmosphereCurves`, `TimeOfDay` (events, `SkyState`) |
| `weather.hpp` | `WindField` (+`WindGpu`), `WeatherController`, `WeatherPreset` |
| `water.hpp` | `GerstnerWaves` (+`GerstnerParamsGpu`), `computeBuoyancy`, `BuoyancySettings` |
| `streaming.hpp` | `ChunkStreamer`, `ChunkCallbacks`, `IChunkExecutor`, `ThreadPoolExecutor`, `InlineExecutor` |
| `chunk_data.hpp` | `ChunkData` (heightfield tile + splat + vegetation + user blob), `serializeChunk`/`deserializeChunk` |
| `physics_bridge.hpp` | header-only `toShapeDesc(PhysicsHeightfieldTile)`, `toShapeDesc(VegetationCollider)` |

GLSL (include root `engine/shaders`): `world/gerstner.glsl`, `world/wind.glsl`, `world/preetham.glsl`,
`world/cdlod.glsl`. A test compiles all of them with shaderc.

## Terrain data

```cpp
using namespace ox::world;
HeightfieldDesc d;
d.resolution = 2049;            // samples per side → 2048 quads
d.worldSize = 4096.f;           // metres → 2 m spacing
d.heightScale = 600.f;          // world height = heightOffset + normalized * heightScale
d.origin = {-2048.f, -2048.f};  // world XZ of sample (0,0)
d.format = HeightFormat::Float32; // or UNorm16 (R16_UNORM on the GPU)
Heightfield hf(d);

TerrainNoiseSettings ns;
ns.fractal = {.basis = NoiseBasis::Simplex, .type = FractalType::Ridged, .seed = 7,
              .frequency = 1.f / 1500.f, .octaves = 7, .warpStrength = 120.f};
generateNoise(hf, ns);                                   // world-space noise: tiles match at borders
erodeHydraulic(hf, {.droplets = 300000, .seed = 1});      // mass-conserving droplets
erodeThermal(hf, {.iterations = 30, .talusAngleDeg = 38.f});
terrace(hf, 12, 0.6f, 0.3f);                              // optional

f32 h = hf.sampleHeight({x, z});      // bilinear, metres
glm::vec3 n = hf.sampleNormal({x, z});
f32 slope = hf.sampleSlope({x, z});    // radians

auto imported = Heightfield::load("terrain.png", {.worldSize = 4096.f, .heightScale = 600.f}); // .png16/.r16/.raw/.exr
```

* Samples are stored **normalized**; `setScale()` rescales heights without touching data. EXR values are
  metres. `savePng16` / `saveRaw16` export; `extractR16(rect)` gives GPU upload data for a dirty rect.
* Holes are per sample (`setHole`); a quad is a hole if any corner is. Physics tiles mark them with
  `kPhysicsHeightHole`.
* Hydraulic erosion normalises heights by the relief internally, so parameters don't depend on scale. By
  default sediment of droplets reaching the border is deposited (exact mass conservation); set
  `loseSedimentAtBorder` for open boundaries.

### Brushes, splat maps

```cpp
BrushSettings b{.op = BrushOp::Raise, .radius = 15.f, .strength = 4.f, .falloff = BrushFalloff::Smooth};
IRect dirty = applyBrush(hf, cursorXZ, b, dt);      // exact bounding rect of modified samples
quadtree.updateBounds(hf, dirty);                    // CDLOD min/max
for (glm::ivec2 t : physicsTilesOverlapping(dirty, 64, hf.resolution()))
    rebuildCollider(buildPhysicsTile(hf, t.x, t.y, 64));
gpuUpload(dirty, hf.extractR16(dirty));

SplatMap splat(1025, 4, d.origin, d.worldSize);      // layer 0 = base
SplatRule rules[] = {
    {.layer = 1, .minSlopeDeg = 35.f, .maxSlopeDeg = 90.f},                  // rock
    {.layer = 2, .minHeight = 420.f, .heightBlend = 30.f, .maxSlopeDeg = 30.f}, // snow
};
autoPaint(splat, hf, rules);                         // later rules overlay earlier ones; weights sum to 255
paintSplat(splat, cursorXZ, 3, b, dt);
auto tex0 = splat.packRgba8(0, splat.fullRect());     // layers 0-3, tex1 = layers 4-7
```

## Terrain LOD — CDLOD

CDLOD (Strugar 2010) was chosen over geometry clipmaps: per-node min/max bounds give tight culling, LOD
transitions are continuous (vertex morphing, no stitching), a single static grid mesh is instanced for every
patch, and selection is cheap.

```cpp
TerrainLodSettings ls{.leafNodeSize = 32, .lodCount = 7, .viewDistance = 6000.f};
TerrainQuadtree tree(hf, ls);           // warns if ranges are too short to be crack-free
TerrainGridMesh grid = generateTerrainGrid(ls.leafNodeSize, /*skirts*/ true);

TerrainSelection sel;
tree.select({.cameraPosition = camPos, .frustum = Frustum::fromViewProjection(viewProj)}, sel);
std::vector<TerrainPatchGpu> instances;
for (const TerrainPatch& p : sel.patches) instances.push_back(toGpu(p));
tree.debugDraw(sel, debugLine);
```

* LOD 0 = finest; node size = `leafNodeSize << lod` quads. `LodRanges::compute` uses Strugar's
  distribution (`detailBalance` = ratio between successive bands); `morph[l] = (start, end = range[l])`.
* A node whose children are only partly in range is emitted once with `quadrantMask` (bit q = quadrant
  `(zHalf << 1) | xHalf`); the grid mesh's index buffer stores the four quadrants contiguously
  (`TerrainGridMesh::quadrants`), so the renderer draws whole nodes with the full range and partial ones
  per quadrant sub-range.
* Crack-free condition (tested): at every boundary between LOD l and l+1, LOD-l vertices are fully morphed
  and LOD-(l+1) vertices are not morphing. Holds when `range[l-1] + nodeDiagonal(l-1) ≤ morphStart[l]`
  (checked with a warning at build; with defaults: viewDistance ≳ 1.2 · leafWorldSize · (2^lodCount − 1)).
* Physics: `buildPhysicsTiles(hf, 64)` → tiles of 65×65 samples sharing edges, heights in metres, matching
  `ShapeDesc::heightField` (`toShapeDesc` in `physics_bridge.hpp`). A test raycasts a Jolt body built
  from a tile against `sampleHeight`.

## Vegetation

```cpp
VegetationLayer pine{.name = "pine", .kind = VegetationKind::Tree, .prototype = 0, .minDistance = 6.f,
                     .maxSlopeDeg = 30.f, .splatLayer = 0, .minScale = 0.8f, .maxScale = 1.3f,
                     .boundingRadius = 6.f, .collider = true, .colliderRadius = 0.4f, .colliderHalfHeight = 3.f};
VegetationLayer grass{.name = "grass", .prototype = 1, .minDistance = 0.4f, .maxSlopeDeg = 45.f,
                      .alignToNormal = 1.f};
grass.lod.cullDistance = 80.f; grass.lod.impostorDistance = 0.f;
VegetationScatterer scatter({pine, grass});
ScatterContext ctx{.heightfield = &hf, .splat = &splat};
ctx.exclusions = roadZones;                                       // circles/rects with layer mask
VegetationChunk chunk = scatter.scatter(chunkOrigin, 128.f, ctx);  // deterministic, seamless across chunks
for (const VegetationCollider& c : chunk.colliders) addStaticBody(c.position, c.rotation, toShapeDesc(c));
std::vector<VisibleVegetationCell> visible;
cullVegetationCells(chunk, scatter.layers(), frustum, camPos, visible);
```

* Placement uses one **tileable (toroidal) Poisson-disk pattern per layer** repeated over the world with a
  per-layer offset, so the minimum distance holds across chunk borders and the result does not depend on
  chunking. Rules reject points (holes, height band, slope band, splat weight, exclusion zones) and
  density maps / `customDensity` thin them (acceptance probability, deterministic per point).
* Instances are sorted into cells (per layer) with AABBs for culling; `selectVegetationLod` gives the mesh
  LOD (0..2), impostor or culled plus a cross-fade factor.

## Sky, sun, moon, time of day

```cpp
SolarPosition sun = computeSunPosition({48.85, 2.35}, {2024, 6, 21, 10.5 /*UTC*/});
glm::vec3 toSun = horizontalToWorld(sun.elevationDeg, sun.azimuthDeg);
CelestialLight light = sunLight(sun.elevationDeg, /*turbidity*/ 2.5f);  // lux + colour after the atmosphere
PreethamSky sky = PreethamSky::compute(toSun, 2.5f);                     // upload sky.toGpu()

TimeOfDay tod({.location = {52.37, 4.90}, .year = 2024, .month = 6, .day = 21, .localHours = 6.0,
               .utcOffsetHours = 2.0, .timeScale = 60.0});
tod.addListener([](TimeOfDayEvent e, const SkyState& s) { if (e == TimeOfDayEvent::Sunset) lightStreetLamps(); });
tod.curves().fogDensity = Curve({{-10.f, 0.02f}, {10.f, 0.004f}}, CurveInterp::Smooth); // by sun elevation
tod.update(realDt);
const SkyState& s = tod.state(); // sun/moon dirs, lights, moon phase, starsRotation, preetham, atmosphere params
```

* Sun: NOAA/Meeus low-precision algorithm, ~0.01° (tests: Meeus example 25.a, NREL SPA reference case,
  equinox/solstice noon elevations). Moon: Schlyter's lunar theory with main perturbations and topocentric
  parallax (~0.3°; tested against the 2024-04-08 eclipse and 2024 quarter/full/new moon times).
* `starsRotation` maps the celestial frame (+X vernal equinox, +Y north celestial pole) to the world —
  rotate the star cubemap with it.
* Events (Sunrise/Sunset at −0.833°, Noon, Midnight) are detected with ≤ 10 game-minute sub-steps.
* Curves are driven by sun elevation (default, works for every latitude/season) or by local hour
  (`CurveDriver::LocalHour`, use `wrap = true`).

## Weather and wind

`WindField::sample(pos, t)` → horizontal wind (m/s) with gust fronts travelling downwind and cross-wind
sway; `toGpu(t)` feeds `world/wind.glsl` (`oxWindSample`, identical math). `WeatherController` blends
`WeatherPreset`s (cloud cover, rain, snow, fog boost, wind) with a smoothstep transition and accumulates
surface wetness and snow cover.

## Water

```cpp
GerstnerWaves waves = GerstnerWaves::fromWind({1, 0}, /*wind m/s*/ 7.f, 8, /*seed*/ 3);
waves.baseHeight = 12.f;
f32 h = waves.heightAt({x, z}, t);     // Eulerian (inverts horizontal displacement)
glm::vec3 n = waves.normalAt({x, z}, t);
GerstnerParamsGpu gpu = waves.toGpu(t); // → world/gerstner.glsl (oxGerstnerDisplacement / Normal / Height)

BuoyancySettings hull = BuoyancySettings::fromBox({1.5f, 0.4f, 4.f}, 4);
BuoyancyResult r = computeBuoyancy(hull, comPos, rot, linVel, angVel,
                                   [&](glm::vec2 xz) { return waves.heightAt(xz, t); });
body.addForce(r.force); body.addTorque(r.torque);
```

Buoyancy samples points (each a small cube with volume/height), computes the submerged fraction per point,
Archimedes force + linear drag relative to the water (optional current) at the point, and angular damping.

## Streaming

```cpp
ChunkCallbacks cb;
cb.load = [&](ChunkCoord c, const std::atomic<bool>& cancelled) -> std::unique_ptr<ChunkPayload> {
    auto data = std::make_unique<ChunkData>();
    if (!deserializeChunk(readFile(c), *data)) return nullptr;   // worker thread
    return data;
};
cb.onLoaded = [&](ChunkCoord c, ChunkPayload& p) { spawn(static_cast<ChunkData&>(p)); }; // main thread
cb.onUnload = [&](ChunkCoord c, ChunkPayload& p) { despawn(c); };
cb.save = [&](ChunkCoord c, ChunkPayload& p) { writeFile(c, serializeChunk(static_cast<ChunkData&>(p))); }; // optional

ChunkStreamer streamer({.chunkSize = 256.f, .loadRadius = 1024.f, .unloadRadius = 1280.f}, cb /*, executor*/);
StreamingViewer viewers[] = {{camPos, camForward, 1.f, /*id*/ 0}};
streamer.update(viewers);                       // per frame
if (!streamer.isAreaReady(playerPos, 300.f)) showLoadingScreen();
streamer.debugDraw(debugLine, 0.f);
```

* Per chunk: `Unloaded → Loading → Loaded → (Unloading while `save` runs) → Unloaded`.
* Priority = distance to the chunk rect × (1 − viewDirectionWeight × facing); nearest first. Budgets:
  requests per update, max in-flight loads, activations (`onLoaded`) per update, unloads per update.
* Hysteresis: load inside `loadRadius`, unload beyond `unloadRadius`. Loads that leave range are cancelled
  (flag passed to the loader; late results discarded). A viewer jump > `teleportDistance` cancels stale loads
  and multiplies the budgets for 30 updates. Failed loads (nullptr / exception) retry after
  `failedRetryUpdates`.
* Executor: `IChunkExecutor` (integration can adapt `core` jobs); default `ThreadPoolExecutor(2)`;
  `InlineExecutor` for tools/tests. The executor must eventually run every submitted job — the streamer's
  destructor waits for them (call `unloadAll()` first if `onUnload` must run).
* `ChunkData` binary format: `"OXCH"`, version, coord, tagged sections (`HFLD`, `HOLE`, `SPLT`, `VEGI`,
  `USER`) each with a CRC32; unknown sections are skipped, corruption is detected.

## Renderer contract (GPU side, to be implemented by the render team)

* **Terrain feature**: heightmap texture (R16_UNORM or R32F from `Heightfield::rawBytes`, partial updates via
  dirty rects), splat textures (2× RGBA8), hole mask; instanced draw of `TerrainGridMesh` with
  `TerrainPatchGpu` instances (32 B); vertex shader uses `world/cdlod.glsl`, samples height at the unmorphed
  and morphed position, pushes skirt vertices down by `TerrainQuadtree::skirtDepth(lod)`; hole → discard.
  The CPU selection can later move to a compute pass (same algorithm).
* **Vegetation feature**: per chunk an instance buffer of `VegetationInstanceGpu` (64 B; rows of a 3×4
  matrix: `vec3 world = vec4(local, 1) * mat3x4(t0, t1, t2)`, same layout as `VkTransformMatrixKHR`),
  GPU frustum/distance culling + LOD/impostor selection per instance (cells give a CPU pre-cull), dithered
  cross-fade (`fade`), wind sway from `world/wind.glsl` using `random` as phase, shadows for trees
  (`kVegFlagCastsShadow`). Dense grass can instead be generated on the GPU around the camera with the same
  rules.
* **Sky feature**: Preetham sky from `PreethamSky::Gpu` (`world/preetham.glsl`) by day, stars cubemap rotated
  by `SkyState::starsRotation` scaled by `atmosphere.starsIntensity`, moon disc lit by the sun direction
  (phase falls out of the geometry; `moonPhase` for brightness), sun disc. Main directional light =
  `mainLightDirection/Color/Illuminance`; fog/ambient/exposure from `SkyState::atmosphere`.
  (A Bruneton/Hillaire LUT sky can replace Preetham later with the same inputs.)
* **Water feature**: grid displaced in the vertex shader with `oxGerstnerDisplacement` / `oxGerstnerNormal`
  from `GerstnerParamsGpu` (528 B, std140) — identical math to the CPU so floating objects match.

## Known limits / TODO

* Single heightfield per terrain (no multi-tile virtual heightmap / clipmap texture streaming yet); nodes
  overhang the border if `(resolution − 1)` is not a multiple of `leafNodeSize`.
* Hydraulic erosion is single-threaded (~0.3 s per 100k droplets on 129² — fine for tools, not runtime).
* Moon accuracy ~0.3°; no nutation/aberration for the moon, no eclipses dimming.
* Preetham is invalid for a sun below the horizon (clamped to 1°); twilight relies on the curves.
* Streamer chunks are 2D (XZ); no vertical layers. Not yet bound to `core` jobs, reflection or ECS
  components (integration step).
