# Module `ai` (`Oxwald::ai`)

Navigation (Recast/Detour/DetourCrowd/DetourTileCache 1.6 from vcpkg), behaviour trees with a blackboard,
perception (sight/hearing/memory) and a small utility-AI scorer. Depends only on `core` + glm + nlohmann-json.
Detour types appear in public headers only as forward-declared handles (`detour()` accessors).

## Headers

| Header | Contents |
| --- | --- |
| `oxwald/ai/nav_types.hpp` | `NavMeshInput` (triangle soup + per-tri areas, off-mesh links, convex area volumes), `NavMeshBuildSettings`, `NavArea`, `NavFlags`, `NavQueryFilter`, `DebugLineFn` |
| `oxwald/ai/navmesh.hpp` | `NavMesh` — solo/tiled build, tile rebuilds, (de)serialisation, poly flags, debug draw |
| `oxwald/ai/nav_query.hpp` | `NavQuery` — path (corridor + string pulled), nearest point, raycast, random point (radius), smooth path |
| `oxwald/ai/nav_crowd.hpp` | `NavCrowd`, `NavAgent`, `NavAgentParams` |
| `oxwald/ai/nav_tile_cache.hpp` | `NavTileCache` — dynamic obstacles (cylinder/box/oriented box) |
| `oxwald/ai/blackboard.hpp` | `Blackboard`, `BlackboardValue` |
| `oxwald/ai/behavior_tree.hpp` | BT nodes, `BehaviorTree`, `BTBuilder`, `BTFactory` (JSON) |
| `oxwald/ai/perception.hpp` | `PerceptionSystem`, `TeamAttitudes`, sight/hearing configs, `NoiseEvent` |
| `oxwald/ai/utility_ai.hpp` | `UtilityScorer`, `ResponseCurve` |

## Navmesh

```cpp
using namespace ox::ai;
NavMeshInput in;                        // vertices (vec3), indices (u32), triAreas (optional)
in.addQuad({-50,0,-50}, {-50,0,50}, {50,0,50}, {50,0,-50});   // CCW seen from above = walkable side up
in.addBox({-1,0,-5}, {1,3,5});
in.offMeshLinks.push_back({.start = {10,0,0}, .end = {14,2,0}, .radius = 0.5f, .area = NavArea::Jump});
in.volumes.push_back({.points = {...}, .minY = -1, .maxY = 1, .area = NavArea::Water});

NavMeshBuildSettings s;                 // cell size/height, agent radius/height/climb/slope, region sizes,
s.agentRadius = 0.4f;                   // edge length/error, verts per poly, detail sampling, partition
s.tiled = true; s.tileSize = 64;        // tiled for big worlds / streaming
auto mesh = NavMesh::build(in, s);

mesh->rebuildTiles(newInput, changedMin, changedMax);   // tiled: only overlapping tiles are rebuilt
std::vector<u8> blob = mesh->serialize();                // header + settings + Detour tile data
auto loaded = NavMesh::deserialize(blob);
mesh->setPolyFlags(doorPoly, NavFlags::Disabled);        // e.g. close a door
```

Areas → flags table (`settings.areaFlags`): Ground/Road/Grass → `Walk`, Water → `Swim`, Door → `Walk|Door`,
Jump → `Jump`. `NavQueryFilter` holds per-area costs and include/exclude flag masks.

## Queries

```cpp
NavQuery q(*mesh);
NavPath p = q.findPath(start, goal, filter);   // status Complete / Partial (unreachable → closest point) / Failed
p.corridor;  p.points;  p.pointFlags;  p.length();
auto n   = q.nearestPoint(pos);                 // optional<NavPoint{position, poly}>
auto hit = q.raycast(a, b);                     // hit, t, position, normal
auto r   = q.randomPointInRadius(center, 5.f, seed);
auto pts = q.smoothPath(start, goal, filter, 0.5f);   // surface-following, handles off-mesh links
```

## Crowd

```cpp
NavCrowd crowd(*mesh, /*maxAgents*/128, /*maxRadius*/1.f);
NavAgent a = crowd.addAgent(pos, {.radius = 0.4f, .maxSpeed = 3.5f, .obstacleAvoidanceQuality = 3});
crowd.setTarget(a, goal);
crowd.update(dt);                               // fixed update
glm::vec3 p = crowd.position(a), v = crowd.velocity(a);
crowd.reachedTarget(a, 0.3f);
```

## Dynamic obstacles (DetourTileCache — available in the vcpkg build)

```cpp
auto cache = NavTileCache::build(in, {.build = s, .maxObstacles = 256});
auto id = cache->addCylinder(pos, 1.f, 2.f);    // or addBox / addOrientedBox
cache->update(dt);                               // re-triangulates affected tiles (flush() for loading)
cache->removeObstacle(id);
NavQuery q(cache->navMesh());  NavCrowd crowd(cache->navMesh());
```
Layers are stored uncompressed (no FastLZ dependency). Tile-cache navmeshes are not serialisable yet.

## Behaviour trees

Statuses `Running/Success/Failure` (`Idle` = not ticked). Nodes: `Sequence`, `Selector`, `Parallel`
(RequireOne/RequireAll policies), `Inverter`, `Succeeder`, `Repeater`, `Cooldown`, `Timeout`,
`BlackboardCondition` (decorator or leaf), `Condition` (lambda), `Action` (lambda + optional abort callback), `Wait`,
`SetBlackboard`, `SubTree`. Custom nodes derive from `BTNode`/`BTComposite`/`BTDecorator`.

```cpp
auto tree = BTBuilder()
    .selector("Root")
        .blackboardCondition("enemy", BBOp::IsSet, {}, BTAbortMode::Both)   // UE-style observer abort
            .sequence("Combat")
                .action("Aim", aim)
                .cooldown(0.5f).action("Fire", fire)
            .end()
        .sequence("Patrol").action("MoveToWaypoint", move, cancelMove).wait(2.f).end()
    .end()
    .build(blackboard);                 // shared_ptr<Blackboard>, optional
tree->setUserData(&entityController);   // available as BTContext::user
tree->tick(dt);
```

* `BTAbortMode::Self` aborts the guarded branch when its key changes and the condition becomes false;
  `LowerPriority` aborts a running lower-priority sibling (of the parent `Selector`) when it becomes true.
  Driven by blackboard observers (only re-evaluated when a watched key changed).
* Blackboard: `set/get<T>/getOr/erase`, types `bool, i32, f32, std::string, glm::vec3, u64 (ids)`,
  `observe(key or "" for all, fn(key, old, new))`, `version()`, JSON save/load.
* Tracing: `tree->trace()` → per node `{id, parentId, depth, name, type, status, lastTick, tickedThisFrame}`
  for an editor visualiser; `setTraceCallback` for live logging.
* Timing: `Wait`/`Timeout` accumulate `dt` including the entering tick; `Cooldown` uses tree time.

### JSON

```json
{ "blackboard": { "angry": false },
  "root": { "type": "Selector", "children": [
     { "type": "BlackboardCondition", "key": "angry", "op": "Equals", "value": true, "abort": "LowerPriority",
       "child": { "type": "Action", "action": "Attack" } },
     { "type": "Repeater", "count": -1, "child": { "type": "SubTree", "tree": "Idle" } } ] } }
```
```cpp
BTFactory f;                                    // built-ins pre-registered
f.registerAction("Attack", attackFn);
f.registerCondition("IsAngry", fn);
f.registerTree("Idle", idleJson);               // SubTree targets
f.registerNode("MyNode", [](const json& j, const BTFactory&) { return std::make_unique<MyNode>(...); });
auto tree = f.load(json);  BTFactory::save(*tree->root());  // or loadFile / saveFile
```
Ops: `IsSet, IsNotSet, Equals, NotEquals, Less, LessOrEqual, Greater, GreaterOrEqual` (numeric types compare
numerically). Values: JSON bool/int/float/string, `[x,y,z]` → vec3, `{"id": n}` → u64.

## Perception

```cpp
PerceptionSystem perception;
perception.setRaycast([&](glm::vec3 a, glm::vec3 b) { return physics.blocked(a, b); });
auto guard = perception.addListener({.position = p, .forward = f, .team = 1, .selfId = guardEntity,
                                     .sight = {.range = 20, .fovDegrees = 90}, .hearing = {}});
perception.setSource({.id = playerEntity, .position = playerPos, .team = 2});
perception.reportNoise({.position = shotPos, .loudness = 1, .radius = 30, .instigator = playerEntity, .team = 2});
perception.setListenerTransform(guard, p, f);
perception.update(dt);
for (const PerceivedStimulus& s : perception.perceived(guard)) { s.lastKnownPosition; s.currentlySensed; s.age; }
auto target = perception.bestHostile(guard);
```
Sight: main FOV cone + range, peripheral cone with short range (half strength), lose-sight range hysteresis,
LOS raycast eye → target aim point, memory keeps the last known position for `forgetAfter` seconds while strength
decays. Hearing: distance falloff inside `radius × rangeMultiplier`, optional occlusion attenuation, threshold.
Teams: `TeamAttitudes` (same team friendly, different hostile, team 0 neutral; overridable); listeners choose
which attitudes they report. Gain/loss events via `setEventCallback`.

## Utility AI (bonus)

`UtilityScorer` with considerations (input fn → `ResponseCurve` Linear/Polynomial/Logistic/Logit), product with
compensation factor, `best(bb, current, stickiness)`.

## Tests (`ox_ai_tests`)

Wall detour (no crossing), partial path for disconnected islands, off-mesh link + flag exclusion, water area/flags
& runtime poly flags, nearest/height/raycast/random queries, tiled build + tile rebuild after adding/removing a
wall, serialisation round trip (solo + tiled, byte-identical re-save), crowd (8 crossing agents reach goals with
min separation), tile-cache box/cylinder obstacles, blackboard types/observers/JSON, BT sequence/selector/parallel/
decorators/timeouts, observer aborts (Both + none), comparisons & trace, JSON load/save/subtree/errors, perception
cone/peripheral/LOS/teams/forgetting/hysteresis/hearing/occlusion, utility scorer.

## Known limits / TODO

* `NavMesh::rebuildTiles` brute-force filters triangles per tile (no chunky tri-mesh BVH) — fine for moderate
  scenes; add a spatial index for huge worlds. The XZ tile grid is fixed at build time.
* Tile-cache meshes: no serialisation; layers uncompressed.
* `NavQuery` is not thread-safe (one per thread). Random queries use a thread-local RNG (Detour API limitation).
* Perception is O(listeners × sources) per update — add spatial partitioning / staggered updates for big crowds.
* Lower-priority aborts apply to direct children of a `Selector` (UE semantics), not deeper descendants.
