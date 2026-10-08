# Module `spline`

CPU-only curve library: Bézier / Catmull-Rom / B-spline / NURBS / polyline curves, arc-length
parametrisation, rotation-minimizing frames, path following with events, editing helpers for gizmos,
mesh extrusion (roads, pipes) and renderer-independent debug lines. Depends only on `core` (+glm).
Namespace `ox::spline`.

| Header | Contents |
| --- | --- |
| `oxwald/spline/spline.hpp` | `Spline`, `ControlPoint`, `SplineSettings`, `SplineSample`, `SplineMarker`, `frameRotation` |
| `oxwald/spline/path_follower.hpp` | `PathFollower`, `FollowerSettings`, `LoopMode`, `PathEvent`, `FollowerPose` |
| `oxwald/spline/spline_mesh.hpp` | `extrude`, `ExtrusionProfile`, `ExtrusionSettings`, `ExtrudedMesh`, profile makers |
| `oxwald/spline/spline_debug.hpp` | `drawSpline`, `drawCurve`, `drawControlPoints`, `drawHandles`, `drawFrames` (line callback) |
| `oxwald/spline/bezier.hpp` | stateless cubic Bézier helpers (evaluate, de Casteljau, split, Hermite → Bézier) |

## Curve model

```cpp
using namespace ox::spline;
Spline road(SplineType::CatmullRom);          // Linear, Bezier, CatmullRom, BSpline, Nurbs
road.addPoint({0, 0, 0});
road.addPoint({10, 0, -5});
road.addPoint({20, 1, 0});
road.setClosed(false);
SplineSettings s = road.settings();
s.catmullRomAlpha = kCatmullRomCentripetal;   // 0 uniform, 0.5 centripetal (no cusps), 1 chordal
road.setSettings(s);
```

* **Parameter `t` ∈ [0, segmentCount()]**, segment `i` covers `[i, i+1]`. Bézier/Catmull-Rom/Linear: point `i`
  is at `t = i`. B-spline/NURBS: segments are the non-empty knot spans (curve approximates the points; open
  curves are clamped so they start/end at the first/last point). Closed splines wrap `t` and distances,
  open ones clamp. `evaluate(maxT())` on a closed spline equals `evaluate(0)`.
* `ControlPoint`: `position`, `inHandle`/`outHandle` (Bézier, **offsets relative to position**), `handleMode`
  (`Free`, `Aligned`, `Mirrored`, `Auto`), `roll` (radians around the tangent, linearly interpolated),
  `weight` (NURBS), optional `up` (for `FrameMode::UpVector`).
* All Bézier/Catmull-Rom/Linear segments are converted to cubic Bézier segments internally; B-spline/NURBS use
  a rational de Boor evaluator with analytic first/second derivatives (Piegl & Tiller A2.3).
* NURBS: `settings.degree` (clamped to `[1, min(7, n-1)]`), per-point weights, clamped-uniform knots or a
  custom `settings.knots` (open only, size `n+degree+1`). Closed B-spline/NURBS are periodic uniform.
  Exact circle: 9 points, degree 2, weights `1, √½, 1, …`, knots `{0,0,0,¼,¼,½,½,¾,¾,1,1,1}`.
* The evaluation cache (segments, arc-length LUT, frames) is rebuilt lazily after any edit. `version()`
  increments on every edit (use it to invalidate extruded meshes etc.).

## Evaluation and frames

```cpp
SplineSample smp = road.evaluate(1.25f);
smp.position; smp.tangent; smp.normal /* up */; smp.binormal /* right = cross(tangent, normal) */;
smp.derivative; smp.secondDerivative; smp.curvature;
glm::quat q = smp.rotation();                 // maps −Z → tangent, +Y → normal
```

* `FrameMode::RotationMinimizing` (default): double reflection (Wang et al. 2008) on 16 samples per
  segment, seeded with `settings.upVector`; arbitrary `t` is reached with one extra reflection step.
  Closed loops distribute the holonomy angle along the arc length so frames are continuous at the seam.
* `FrameMode::UpVector`: normal = per-point `up` (interpolated) or `settings.upVector`, made ⊥ to the tangent
  (falls back to RMF where the tangent is parallel to up).
* Roll is applied on top of both modes.

## Arc length and queries

```cpp
f32 len = road.length();
f32 t = road.distanceToT(12.5f);              // LUT (16 steps/segment, 5-pt Gauss-Legendre) + Newton
f32 d = road.tToDistance(t);
std::vector<SplineSample> rings = road.sampleByDistance(0.5f);  // equal spacing, ends included
ClosestPointResult hit = road.closestPoint(playerPos);          // {t, position, distance}
Bounds box = road.bounds();                                     // exact for cubic kinds
```

Closest point: segments sorted by distance to their convex-hull AABB, culled once the bound exceeds the best
hit, 17-sample coarse search, golden-section in the bracket, Newton polish.

## Editing (gizmos)

```cpp
Spline path(SplineType::Bezier);
path.addPoint({0, 0, 0}); path.addPoint({5, 0, 0});        // new points are HandleMode::Auto
path.setHandleMode(0, HandleMode::Mirrored);
path.setOutHandle(0, {1, 2, 0});                            // in handle becomes {-1,-2,0}
usize idx = path.insertPointAt(0.4f);                       // de Casteljau split, shape preserved
path.removePoint(idx);
path.addMarker("checkpoint", 0.75f);
```

* Moving a handle enforces the mode on the opposite handle; editing an `Auto` handle converts the point
  to `Aligned`. `Auto` handles are `±(next − prev)/6` (uniform Catmull-Rom tangent → C1) and are recomputed
  whenever positions change.
* `insertPointAt` on Bézier splits exactly (neighbours in `Mirrored`/`Auto` become `Aligned`, markers in the
  split segment are remapped). On other types it inserts the curve point at `t` (shape changes slightly).

## Path following

```cpp
PathFollower f(FollowerSettings{.speed = 5.0f, .loopMode = LoopMode::PingPong});
f.addEvent("horn", 40.0f);                                   // follower events (distances)
f.setEventCallback([](const PathEvent& e) { OX_LOG_INFO("game", "{} dir {}", e.name, e.direction); });
f.advance(road, dt);                                         // distance based → constant world speed
FollowerPose pose = f.pose(road);                            // position, rotation (quat), distance, t
```

* Loop modes `Once` (sets `finished()`), `Loop`, `PingPong`. Negative speed runs backwards.
* Events = follower events + spline markers (`fireMarkers`). Every crossing fires once, in travel order, also
  several per update, across wraps and in both ping-pong directions (half-open intervals; the first move and
  the move after a wrap include the start distance).
* Orientation: `forwardAxis` (default −Z) along travel direction (`faceTravelDirection`), `upAxis` (+Y) along the
  frame normal. Poses are in spline-local space; compose with the owning entity transform.

## Mesh extrusion

```cpp
ExtrudedMesh pipe = extrude(road, makeCircleProfile(0.3f, 12), {.spacing = 0.5f, .capStart = true, .capEnd = true});
ExtrudedMesh strip = extrude(road, makeStripProfile(6.0f), {.spacing = 1.0f, .vPerUnit = 0.1f});
// pipe.vertices: {position, normal, uv}, pipe.indices: u32 triangle list (CCW front faces)
```

Profile x → binormal (right), y → normal (up). Normals point to the right of the profile direction (CCW closed
profiles face outward). Duplicate a profile point for a hard edge. Closed profiles get a seam column; closed
splines duplicate the first ring at the end. Counts: `rings = round(length/spacing)+1`,
`vertices = rings·columns (+ (points+1) per cap)`, `indices = (rings−1)·(columns−1)·6 (+ 3·points per cap)`.

## Debug drawing

```cpp
ox::spline::drawSpline(road, [&](glm::vec3 a, glm::vec3 b, glm::vec4 c) { debugDraw.line(a, b, c); },
                       {.frameSpacing = 2.0f});
```

## Known limits / TODO

* Const queries build the cache lazily under a lock (double-checked atomic flag), so concurrent const queries are
  safe; `rebuild()` just builds eagerly. Copies start with an empty cache. Editing is not thread-safe.
* Arc-length LUT is fixed at 16 steps per segment (accurate to ~1e-6 relative for smooth segments; extremely
  long, kinked segments may need more). No adaptive Simpson fallback yet.
* Closest point searches the 16-sample coarse minimum per segment; very tight loops inside one segment could
  pick a local minimum.
* Markers are stored by parameter `t`; only Bézier `insertPointAt` remaps them; other edits keep the raw `t`.
* Linear splines have frame/tangent discontinuities at corners (by design); extrusion of polylines kinks.
* Centripetal/chordal Catmull-Rom is G1 (not C1) at points; uniform is C1.
* Caps triangulate as a fan from the centroid (convex profiles only); hard-edge duplicates create degenerate
  triangles.
* No serialization/reflection yet (waits for core `reflect`); no ECS components (owned by `gameplay`).
