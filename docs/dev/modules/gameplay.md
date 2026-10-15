# gameplay (`Oxwald::gameplay`, namespace `ox::gameplay`)

ECS glue: components + systems that bind the CPU modules (physics, animation, spline, audio, ai, net, script and,
when configured, async, world and the assets database) to the scene `World`. Public deps: scene + all of those modules. Umbrella header
`<oxwald/gameplay/gameplay.hpp>`.

```cpp
ox::registerSceneTypes();
ox::registerGameplayTypes();                       // reflection + ComponentRegistry (idempotent)
ox::Services services;                             // optional: PhysicsWorld, ScriptVM, audio::AudioEngine, DebugDraw,
ox::gameplay::GameplayAssetRegistry assets;        //   CoroutineScheduler, script::AsyncBridge, providers ...
assets.registerIn(services);
ox::SystemScheduler scheduler;
ox::addGameplaySystems(scheduler, services);       // creates missing services + runtimes, adds the systems
scheduler.attach(world, services);
scheduler.setPlaying(true);                        // physics/scripts/AI/followers only simulate in play mode
scheduler.tick(world, services, dt);
```

`addGameplaySystems(scheduler, services, GameplayConfig)` creates (when missing) `physics::PhysicsWorld`
(`config.physicsWorld`), `script::ScriptVM` (`config.scriptVM`), `EventBus`, and the runtimes, all registered as
services: `PhysicsRuntime`, `AnimationRuntime`, `SplineRuntime`, `AudioRuntime`, `AIRuntime`, `NetworkRuntime`,
`ScriptRuntime`, `CoroutineRuntime` (async), `DestructionRuntime` (`config.destruction`, needs physics). Areas can be switched off in `GameplayConfig`. `TransformSystem` is
added if the scheduler has none. The audio runtime uses an `audio::AudioEngine` service if one is registered;
coroutines need a `CoroutineScheduler` service and the Lua `await` API needs a `script::AsyncBridge` service (both are
created by the engine runtime / test fixtures, not by gameplay).

## Edit vs play mode

`SystemContext::playing` drives everything. The `Gameplay.Lifecycle` system (PreUpdate, -1000) applies transitions at
the start of a frame: entering play creates physics bodies/characters/joints (before scripts, so `onCreate` can use
physics), then script instances; leaving play destroys script instances (`onDestroy`), cancels entity coroutines,
stops audio voices, drops AI state and destroys all bodies. In edit mode nothing simulates; debug visualisation keeps
working (colliders drawn from component data, splines, stored navmesh). Play-in-editor = `world.clone()` + attach the
scheduler to the clone (tested: the edit world is never mutated). Runtime handles live in the runtimes, never only in
components, so cloned/serialized components never carry stale handles.

## Systems and phases

| Phase | Order | System | Mode | Work |
| --- | --- | --- | --- | --- |
| PreUpdate | -1000 | `Gameplay.Lifecycle` | always | attach/detach runtimes, edit<->play transitions |
| PreUpdate | -950 | `Gameplay.Assets.HotReload` | always | apply queued `GameplayAssetEvents` (scripts, trees, prefabs, animators, mesh colliders) |
| PreUpdate | -900 | `Gameplay.Net.Poll` | always | net clock, server/client poll, client interpolation -> transforms |
| PreUpdate | 0 | `Gameplay.Script.PreUpdate` | play | `AsyncBridge::update`, `vm.update(dt)` (timers, coroutines, hot reload), create pending instances |
| PreUpdate | 10 | `Gameplay.Coroutines` | always | `CoroutineScheduler::tick` (`config.tickCoroutines`) |
| FixedUpdate | -100 | `Gameplay.Script.FixedUpdate` | play | `onFixedUpdate` |
| FixedUpdate | -90 | `Gameplay.Net.Predict` | play | PredictedCharacter: client samples/applies/sends inputs, server runs queued inputs + acks |
| FixedUpdate | -70 | `Gameplay.AI.Perception` | play | listeners/sources from transforms, LOS via physics, blackboard `target*` keys |
| FixedUpdate | -60 | `Gameplay.AI.BehaviorTrees` | play | tick trees (`tickInterval`) |
| FixedUpdate | -50 | `Gameplay.AI.Navigation` | play | bake/load navmesh on demand, crowd update, move entities / drive character controllers |
| FixedUpdate | 0 | `Gameplay.Physics.Step` | play | pending bodies, sync kinematic/teleports/scale in, character moves, `step`, dynamic poses out, events |
| FixedUpdate | 5 | `Gameplay.Destruction` | play | contacts of the step → damage/breaks, replicated `broken`/zero health, Static-anchor re-check, debris lifetime/fade, budget |
| FixedUpdate | 10 | `Gameplay.Coroutines.Fixed` | always | `CoroutineScheduler::fixedTick` (after the physics step) |
| Update | 0 | `Gameplay.Script.Update` | play | `onStart`/`onUpdate`; resolves `physics.raycastAsync` |
| Update | 50 | `Gameplay.Spline.Followers` | play | followers advance, events |
| Update | 100 | `Gameplay.Animation` | always | animators (play mode or `animateInEditMode`), root motion, IK, skinning palettes |
| PostUpdate | -1100 | `Gameplay.Physics.Interpolate` | play | interpolated dynamic poses (alpha) before `Transform` (-1000) |
| PostUpdate | 100 | `Gameplay.Audio` | always | listener/sources positions + velocities, autoplay, `AudioEngine::update` |
| PostUpdate | 200 | `Gameplay.Net.Send` | always | server: spawn new identities, viewer positions, snapshots at the tick rate |
| Extract | 0 | `Gameplay.DebugDraw` | always | into the `ox::DebugDraw` service (if registered) |

Component add/remove/destroy is handled through EnTT `on_construct/on_update/on_destroy` signals: changed physics
components recreate the body at the next fixed step (velocities kept), removed ones destroy it; script components
create/recreate instances; animator changes rebuild only when assets/inline controller changed (parameter edits don't
restart the state machine).

### Transforms
Physics reads world transforms (`Entity::worldTransform()`, hierarchy-aware) and writes world poses with
`setWorldTransform` (keeps scale). Dynamic bodies: a pose that differs from what physics wrote last is a teleport
(gameplay code moved the entity). Kinematic bodies follow the transform with `moveKinematic` (velocities push dynamic
bodies). Static bodies are re-posed when moved. World scale x collider scale is baked into primitive sizes (or a scaled
shape for hulls/meshes/height fields); changing it rebuilds the shape. Children of dynamic bodies just follow through
the hierarchy. Interpolation: physics keeps previous/current poses per body; `Physics.Interpolate` writes
`lerp/slerp(prev, cur, ctx.alpha)` (disable per body with `RigidBody.interpolate = false`).

## Component reference

Runtime fields are `NoSerialize` (+`Hidden`/`ReadOnly`); `SaveGame` marks gameplay state for save games; `Replicated`
fields are network properties. Categories/icons (`attr::Category`, `Meta{"icon"}`) are set for the editor.

### Physics (category "Physics")
| Component | Fields |
| --- | --- |
| `RigidBody` | `motionType` (Static/Kinematic/Dynamic), `layer` (name, empty = auto), `mass` (0 = from density), `inertiaOverride` (vec3 diag), `friction`, `restitution`, `linearDamping`, `angularDamping`, `gravityFactor`, `ccd`, `allowSleeping`, `startActive`, `lockAxes` (`physics::lock` bits), `reportContacts`, `interpolate`, `initialLinearVelocity`, `initialAngularVelocity`; runtime `bodyId` |
| `Collider` | `type` (Box/Sphere/Capsule/Cylinder/ConvexHull/Mesh/HeightField/Compound), `halfExtents`, `radius`, `halfHeight`, `convexRadius`, `density`, `mesh` (asset UUID via `IMeshColliderProvider`), `points` (hull), `heights` + `sampleCount` + `heightFieldOffset` + `heightFieldScale`, `children` (`ColliderChild{type, halfExtents, radius, halfHeight, points, position, rotation}`), `offsetPosition`, `offsetRotation`, `scale`, `isSensor`. Without RigidBody the entity is static. Dynamic + Mesh falls back to the hull. |
| `CharacterController` | `height`, `radius`, `maxSlopeAngle` (deg), `maxStepHeight`, `stickToFloorDistance`, `mass`, `maxStrength`, `jumpSpeed`, `airControl`, `layer`; runtime input `desiredVelocity`, `jump` (consumed per step); state `groundState`, `velocity`. Entity position = feet. |
| `Trigger` | `requiredTag` (TagComponent filter), `reportStay`, `once`; runtime `overlapCount`, `fired` (SaveGame). Makes the collider a sensor. |
| `Joint` | `type` (Fixed/Point/Hinge/Slider/Distance/Cone), `target` (EntityRef, empty = world), `anchor` (local), `targetAnchor` (local to target, world when no target), `axis` (local), `limitsEnabled`, `limitMin/Max`, `motorMode`, `motorTarget`, `motorMaxForce`, `minDistance`, `maxDistance`, `springFrequency`, `springDamping`, `coneHalfAngle` (deg), `breakForce`/`breakTorque` (0 = unbreakable); `broken` (SaveGame) |

Events: `PhysicsRuntime::onCollision` (`CollisionEvent{phase Begin/Persist/End, a, b, normal, point, impulse}`),
`onTrigger` (`TriggerEvent{phase Enter/Stay/Exit, trigger, other}`), `onJointBroken`; also published on the `EventBus`.
API: `bodyOf/entityOf/characterOf`, `raycast` (-> entity), `overlapSphere`, `countHits`, `addForce/addImpulse/...`,
`linearVelocity/setLinearVelocity`, `teleport`, `debugDraw` flag + `debugOptions`.

### Destruction (category "Physics", `destruction.hpp`, guide chapter 34)
| Component | Fields |
| --- | --- |
| `Destructible` | fracture `chunkCount`, `seed`, `sourceMesh` (asset; empty = Collider: Box/ConvexHull/Mesh, Sphere/Capsule/Cylinder as hulls), `interiorMaterial`, `interiorUvScale`, `planarRatio`; damage `health` (SaveGame, Replicated), `damageThreshold`, `impulseThreshold`, `impulseDamageScale`; debris `density`, `impactImpulseScale`, `spread`, `debrisLifetime`, `fadeDuration`, `maxActiveDebris`, `debrisLayer`; structure `structural`, `anchor` (None/Bottom/Static), `anchorTolerance`, `breakRadius`, `chunkImpulseThreshold`; `breakSound`; `broken` (SaveGame, Replicated, ReadOnly); runtime (not reflected) `prepared` |
| `DestructionChunk` | runtime marker on spawned chunks (not serializable, not removable): `owner`, `index`, `frozen`, `anchored`; runtime `fracture` |

`DestructionRuntime` (service): play start prepares every destructible (`physics::voronoiFracture` of the source in
entity space with the world scale baked in, cached by `fractureKey`; identical objects share one fracture and one set
of chunk meshes, ids `Uuid::fromName("ox.destruction/<key hex>/<i>")`) and records Static-anchor supports. Break: one
entity per chunk (`RigidBody` + hull `Collider` + `MeshRenderer` {outer, interior} + `DestructionChunk`, unit scale,
velocity `v + ω × r` + impact share within `breakRadius`), the intact entity loses RigidBody/Collider/MeshRenderer but
stays alive (scripts, events, replication). Structural objects keep chunks frozen (Static bodies): hit chunks +
chunks without a path to an anchor through frozen chunks (BFS over `FractureResult::links`) are released; Static
anchors are re-checked two fixed steps after any break (an intact Static-anchored object whose recorded support is
gone breaks at that support). Debris: lifetime × `destruction.DebrisLifetimeScale`, fade (body removed, scale → 0,
destroy) once asleep or after 2× lifetime; per-object `maxActiveDebris` and the global `destruction.MaxActiveDebris`
fade the oldest moving chunks. API: `applyDamage` (also on chunk entities), `fracture`, `applyRadialDamage` (distance
to the bounds of intact objects; releases the whole blast radius), `isBroken`, `chunksOf`, `activeDebrisCount`,
`frozenChunkCount`, `prepare`, `sourceMeshOf`, `setMaxActiveDebris`, `onDestruction` (`DestructionEvent{type
Fractured/ChunksReleased/Collapsed, cause Damage/Impact/Script/Collapse/Replicated, point, impulse, chunks,
totalChunks}`, also on the EventBus and as `onEvent(self, "broken"|"chunksReleased"|"collapsed", n)` to the
object's script), `debugDraw` (support graph). Contacts: Begin impulse ≥ `impulseThreshold` breaks, below it
`impulse × impulseDamageScale` damage; frozen chunks are released by `chunkImpulseThreshold` (siblings never release
each other). Render: `engine/render/src/features/destruction/` (extract hook over `prepared`/`fracture` pointers +
`DestructionMeshes` feature registering `MeshData` in the `GpuResourceCache`; works without services, i.e. in the
editor viewport too). Net: a replicated `broken = true` fractures the client copy (debris is local).

### Animation (category "Animation")
| Component | Fields |
| --- | --- |
| `Animator` | `skeleton` (asset), `controller` (asset; nil = `inlineController`), `inlineController{parameters[{name,type,defaultValue}], states[{name, clip, speed, speedParameter, loop}], transitions[{from ("*"=any), to, parameter, op, threshold, duration, hasExitTime, exitTime}], defaultState}`, `applyRootMotion`, `playbackSpeed` (Replicated), `animateInEditMode`, `parameters` (map name -> value pushed every frame; triggers fire once and reset; SaveGame); runtime `currentState` |
| `SkinnedMesh` | `mesh`, `materials`, `skinningMethod` (Linear/DualQuaternion), `gpuSkinning`, `castShadows`, `visible`; runtime (not reflected) `palette` (`model * inverseBind` per joint) + `paletteVersion` for the renderer. Uses the animator on the same entity or the nearest ancestor. |
| `IK` | `chains[{type TwoBone/Aim, rootJoint, midJoint, endJoint, target (EntityRef), targetOffset, pole, poleOffset, aimAxis, weight, enabled}]` |

Inline controller states can be 1D blend spaces: `blendParameter` (float parameter) + `blendSamples[{clip, position}]`
replace `clip` (e.g. idle 0 / walk 1.4 / run 4.5 m/s by "speed"; used by OxwaldShowcase's mannequins).

Root motion: applied to the local transform (owner space), or converted to `CharacterController.desiredVelocity`
(+ yaw on the entity) when the entity has one. Assets come from `IAnimationAssetProvider`. Events:
`AnimationRuntime::onEvent` (`AnimationEvent{entity, name, payload, weight}`), scripts get `onAnimationEvent`.

### Splines (category "Splines")
| Component | Fields |
| --- | --- |
| `Spline` | `type` (Linear/Bezier/CatmullRom/BSpline/Nurbs), `closed`, `points[{position, inHandle, outHandle, handleMode, roll, weight, up?}]` (entity-local), `catmullRomAlpha`, `degree`, `frameMode`, `upVector`, `markers[{name, t}]`, `drawInEditor`, `drawInGame`, `color` |
| `SplineFollower` | `spline` (EntityRef), `speed` (Replicated, SaveGame), `loopMode` (Once/Loop/PingPong), `orientToPath`, `faceTravelDirection`, `forwardAxis`, `upAxis`, `offset` (path frame), `playing` (Replicated, SaveGame), `fireMarkers`, `events[{name, distance}]`, `startDistance`; state `distance`, `direction`, `finished` (SaveGame) |

Constant world speed along the arc length of the spline entity (its world transform applies; speed is in spline-local
units). Events: `SplineRuntime::onEvent` (`SplineFollowerEvent{follower, name, distance, direction, marker}`), scripts
get `onSplineEvent(self, name, distance)`. `SplineRuntime::splineOf/positionAtDistance/rotationAtDistance/
closestDistance/length`.

### Audio (category "Audio")
| Component | Fields |
| --- | --- |
| `AudioSource` | `clip` (asset via `IAudioClipProvider`), `clipPath` (fallback, cached `loadSound`), `stream`, `bus`, `volume`, `pitch` (Replicated), `loop`, `playOnStart`, `spatial`, `attenuation`, `minDistance`, `maxDistance`, `rolloff`, `coneInnerAngle/coneOuterAngle` (deg), `coneOuterGain`, `dopplerFactor`, `distanceLowPass`, `occlusion`, `priority`, `fadeInSeconds`; runtime `playing` |
| `AudioListener` | `active` (first active listener wins) |

Positions/velocities (finite differences) and directions come from world transforms every frame. `occlusion = true`
uses `PhysicsOcclusionProvider` (installed on the engine): bodies between listener and source (other audio entities
ignored), `1 - (1 - 0.6)^hits`. API: `AudioRuntime::play/stop/handleOf/playOneShot`.

### AI (category "AI")
| Component | Fields |
| --- | --- |
| `NavMeshSurface` | Recast settings (`cellSize`, `cellHeight`, `agentHeight`, `agentRadius`, `agentMaxClimb`, `agentMaxSlope`, `regionMinSize`, `regionMergeSize`, `edgeMaxLen`, `edgeMaxError`, `vertsPerPoly`, `detailSampleDist`, `detailSampleMaxError`, `tiled`, `tileSize`), sources (`includeStaticColliders`, `includeMeshes` (MeshRenderer via `IMeshColliderProvider`), `onlyChildren`), `bakeOnStart`, `dynamicObstacles` (tile cache), `drawInEditor`, `bakedData` (hidden, serialized `NavMesh::serialize`); runtime `baked` |
| `NavAgent` | `radius`, `height`, `maxSpeed`, `maxAcceleration`, `separationWeight`, `avoidanceQuality`, `stoppingDistance`, `updatePosition`, `updateRotation`, `driveCharacterController`; state `destination`/`hasDestination` (SaveGame), `reached`, `velocity` |
| `NavObstacle` | `shape` (Cylinder/Box), `radius`, `height`, `halfExtents`, `moveThreshold` (carves surfaces with `dynamicObstacles`) |
| `BehaviorTree` | `tree` (asset via `IBehaviorTreeProvider`), `treeJson` (inline `BTFactory` JSON), `blackboard[{key, type Bool/Int/Float/String/Vec3/Entity, ...Value}]`, `enabled` (SaveGame), `tickInterval`, `restartOnFinish`; runtime `status` |
| `Perception` | `team` (Replicated), `listener`, `source`, `visible` (Replicated), `sight` (`ai::SightConfig`), `hearing` (`ai::HearingConfig`), `detectHostile/Neutral/Friendly`; runtime `target` (best hostile), `targetVisible` |

Baking (`AIRuntime::bake(surface)`, also in edit mode; `gatherNavMeshInput()` is public): static colliders (boxes
exact; spheres/capsules/cylinders/hulls/compound children as oriented boxes; meshes and height fields as triangles) and
static MeshRenderer meshes. Gameplay BT nodes (registered in `AIRuntime::factory()`, usable from JSON):
`MoveTo {target:[x,y,z] | key, acceptance}` (key = vec3 or entity id; re-paths for moving entities, aborts stop the
agent), `PlayAnimation {trigger | state}`, `PlaySound {clip, volume, spatial}`, `IsTargetVisible {key="target"}`
(writes `key` + `keyPosition`), `ScriptAction {function}` (calls the entity's Lua function: `true`/nil = Success,
`false`/"failure" = Failure, "running" = Running) + built-ins (`Wait`, `SetBlackboard`, ...). Blackboards get `self`
(entity id) and perception writes `target`, `targetPosition`, `targetVisible`. Entity ids in blackboards are runtime
ids (`toBlackboardId(entity)`). Events: `AIRuntime::onPerception`, scripts get `onTargetSensed/onTargetLost(self,
source, "sight"|"hearing")`.

### Scripting (category "Scripting")
| Component | Fields |
| --- | --- |
| `Script` | `script` (Lua path relative to the VM search roots, or a name from `IScriptSourceProvider`), `asset` (script asset id, wins), `properties` (map name -> `ScriptPropertyValue{type, number, boolean, text, vector}` overriding the script's declared properties; applied on create and on change, coerced/clamped), `enabled` (Replicated, SaveGame) |

### Networking (category "Networking")
| Component | Fields |
| --- | --- |
| `NetworkIdentity` | `netType` (prefab name the client spawns), `relevancy`, `relevancyRadius`, `priority`, `viewer`; runtime `netId`, `owner` (peer, set on the server before replication) |
| `NetworkTransform` | `syncPosition`, `syncRotation`, `syncScale`, `positionRange`, `positionResolution`, `rotationBits`, `interpolate`, `predicted`, `correctionThreshold` |

| `PredictedCharacter` | `moveAction` ("Move", Axis2D), `jumpAction` ("Jump"), `moveSpeed`, `correctionTolerance` (m), `inputRedundancy`; runtime `corrections`, `pendingInputs` |

**Client prediction with input replay** (`PredictedCharacterComponent` + `CharacterController` + `NetworkIdentity`
with `owner` = the controlling client + `NetworkTransform.predicted`): each fixed step the owning client samples a
`CharacterInput{move, yaw, jump}` from the `ICharacterInputSource` service (the engine implements it with
InputSystem actions, relative to the primary camera yaw; tests register scripted sources), simulates it immediately
through `PhysicsWorld::moveCharacter` (`net::ClientPrediction`) and sends the newest unacknowledged inputs
(`UnreliableSequenced`, exact floats). The server runs them in order (`net::ServerInputQueue`, <= 4 per step, dt
clamped) with the same function and acks `(last seq, position, velocity)` to the owner; a mismatch above
`correctionTolerance` rewinds the character and replays the pending inputs. Both sides tag the entity
`ExternalCharacterMotionTag` so the physics step does not also move it (teleports by gameplay code are still
honoured: the server continues from the moved position, the client is corrected by the next ack). Other clients
interpolate it as usual. Without networking (or for a server-owned character on a host) the input drives
`CharacterController.desiredVelocity/jump`. Tested: immediate local response under 60 ms latency, convergence
< 2 cm without loss and < 5 cm with 20 % loss + jitter, replay after a server-side teleport.

`NetworkRuntime::startServer(transport, NetServerConfig)` / `connect(transport, host, port)`: the server replicates
every `NetworkIdentity` entity (also ones created later; destroyed ones despawn). Properties: world transform
(quantised), then every reflected `attr::Replicated` field of the entity's components sorted by component name/field
order (bool, ints, floats — `Range`+`Step` select a quantised codec —, strings, enums, vecN, quat, nested structs,
`EntityRef` as net id). Clients spawn `netType` through `IPrefabProvider` (or an empty entity), tag it with
`NetworkProxyTag` (physics bodies become kinematic) and interpolate transforms with `InterpolationBuffer` at
`NetClient::renderTime()`. The network clock advances with the frame delta (`setTime` for external clocks).

## Lua entity API

Every script instance has `self.entity`. Assigning a runtime (`NoSerialize`) field through a component proxy, e.g.
`cc.desiredVelocity`/`cc.jump`, does not fire the change signal (it would recreate the physics character every step). Errors raised by the API (invalid entity, unknown field, wrong type) are Lua
errors (logged with traceback, catchable with `pcall`).

```lua
properties = { speed = { type = "float", default = 4, min = 0, max = 20 } }

function onStart(self)
    local e = self.entity
    e.name = "Hero"                                  -- also e.id (uuid string), e.active, e.parent
    local t = e.transform
    t.position = vec3(0, 1, 0)                       -- local; t.rotation (quat), t.scale (vec3 or number)
    t.worldPosition = vec3(3, 1, 0)                  -- world; t.worldRotation
    t:translate(vec3(0, 0, -1))                      -- world space; t:translate(v, "local") along own axes
    t:rotate(vec3(0, 1, 0), math.pi / 2)             -- or t:rotate(quat)
    t:lookAt(scene.find("Target"))                   -- entity or vec3 (optional up)
    print(t.forward, t.right, t.up)                  -- world axes (-Z forward)

    local rb = e:get("RigidBody")                    -- reflected component proxy (nil when absent)
    rb.mass = 20                                     -- any reflected field; fires change signals
    rb.motionType = "Kinematic"                      -- enums by name
    local col = e:get("Collider")
    col.halfExtents = vec3(1, 2, 1)                  -- vec/quat fields are values: assign the whole vector
    col.children = { { type = "Sphere", radius = 0.5, position = vec3(0, 1, 0) } }
    col.children[1].radius = 0.75                    -- nested structs/arrays (1-based)/maps resolve to proxies
    print(#col.children, col:_get("children[0].radius"), col:_type())
    col:_set("offsetPosition.y", 0.5)                -- path access; col:_fields(), col:_value() (plain table)
    e:add("SplineFollower", { speed = 3, spline = scene.find("Rail") })   -- EntityRef fields take entities
    if e:has("Trigger") then e:remove("Trigger") end

    e.body:addImpulse(vec3(0, 5, 0))                 -- also addForce, addTorque, addImpulse(v, point), teleport(p[, q])
    print(e.body.linearVelocity)                     -- read/write; angularVelocity
    e.agent:moveTo(vec3(10, 0, 0))                   -- nav agent: stop(), reached(), velocity, destination
    e.animator:setFloat("speed", 2)                  -- setBool, setTrigger, play(state[, fade]), currentState
    local len = scene.find("Rail").spline:length()   -- positionAt(d), rotationAt(d), closestDistance(p)
    e:sendEvent("hit", { damage = 10 })              -- -> onEvent(self, name, payload) of that entity's script
    local other = e:script()                         -- `self` table of the entity's script (or nil)
    for _, c in ipairs(e:children()) do print(c.name) end
    local hand = e:findChild("Hand")                 -- depth-first by name; e:setParent(other or nil); e:hasTag("x")
end

function onUpdate(self, dt) self.entity.transform:translate(vec3(self.speed * dt, 0, 0), "local") end
function onTriggerEnter(self, other) log.info("entered by", other.name) end      -- also onTriggerExit/Stay
function onCollisionEnter(self, other, info) print(info.normal, info.point, info.impulse) end  -- Stay/Exit
function onSplineEvent(self, name, distance) end
function onAnimationEvent(self, name, payload) end
function onTargetSensed(self, source, sense) end    -- onTargetLost
function onDestroy(self) print(self.entity.name) end -- the entity and its components are still alive here
```

Global API tables:

| Table | Functions |
| --- | --- |
| `scene` | `find(nameOrUuid)`, `findAll(componentName)`, `create([name[, parent]])`, `spawn(prefabNameOrPath[, pos[, rot]])` (IPrefabProvider or document file), `destroy(entity)`; async: `nextFrame()`, `delay(seconds)` |
| `physics` | `raycast(origin, dir[, maxDist[, ignoreEntity]])` -> `{entity, point, normal, distance}` or nil, `overlapSphere(c, r)` -> entities, `gravity()`, `setGravity(v)`; async: `raycastAsync(origin, dir[, maxDist])` |
| `audio` | `play(clipPath[, position[, volume]])` -> id or nil, `stop(id[, fade])`, `isPlaying(id)`, `playSource(entity)`, `stopSource(entity[, fade])` |
| `destruction` | `applyDamage(e, amount[, point[, impulse]])` -> broke, `fracture(e[, point[, impulse]])`, `explode(center, radius, damage[, impulse])` -> affected, `isBroken(e)`, `health(e)`, `chunks(e)`, `activeDebris()`, `setDebugDraw(on)` |
| `ai` | `findPath(a, b)` -> vec3 list, `moveTo(entity, pos)`, `reportNoise(pos[, loudness[, radius[, instigator[, tag]]]])`, `blackboardSet(entity, key, value)`; async: `moveToAsync(entity, pos)` (Future<bool>: arrived) |

Async functions return `Future` userdata (`script::AsyncBridge`); inside `spawn()`ed coroutines:

```lua
function onStart(self)
    spawn(function()
        local arrived = await(ai.moveToAsync(self.entity, vec3(5, 0, 0)))
        local hit = await(physics.raycastAsync(self.entity.transform.worldPosition, vec3(0, -1, 0), 10))
        await(scene.delay(0.5))
    end)
end
```

## Coroutines (async module)

`startCoroutine(services, entity, Task<>, name)` spawns on the `CoroutineScheduler` service with owner
`coroutineOwner(entity)` (= `ox::entityRuntimeId(handle)` from `<oxwald/scene/runtime_id.hpp>`, the same id the
engine runtime uses on its destroy/unload paths; `toRuntimeId` is the same packing); `stopAllCoroutines(services, entity)`. `CoroutineRuntime` cancels an entity's coroutines
when it is scheduled for destruction (`PendingDestroyTag` construct -> before any component is freed; also on
`destroyImmediate`) and all entity coroutines when play mode stops. Script coroutines from C++:
`ScriptRuntime::startScriptCoroutine(entity, "fn")` (`AsyncBridge::invoke`, owned by the instance). Script instances
of destroyed entities get `onDestroy` immediately at `Entity::destroy()` (deferred to the end of the current Lua call
when a script destroys an entity from Lua), so components are still accessible.

## Providers and the asset database

`IMeshColliderProvider` (mesh triangles for mesh colliders and navmesh baking), `IAnimationAssetProvider` (skeletons,
controllers, clips), `IBehaviorTreeProvider` (JSON), `IPrefabProvider` (prefab documents by name + `prefabNames()` for
network types), `IScriptSourceProvider` (sources by name/id), `IAudioClipProvider` (SoundId by asset id),
`IHeightmapProvider` (`HeightmapData{resolution, normalized}` for terrains). `GameplayAssetRegistry` implements them
in memory (tests, tools); `registerIn(services)` only fills interfaces not yet present.

**Asset database bridge** (`engine/gameplay/assets/`, compiled when the `assets` module is configured,
`OX_GAMEPLAY_HAS_ASSETS=1`, header `<oxwald/gameplay/asset_providers.hpp>`). Dependency direction gameplay -> assets:
the asset database knows nothing about gameplay, gameplay owns the interfaces and adapts `assets::AssetManager`:

```cpp
ox::assets::AssetRegistry registry(projectDir);            // or a PakAssetSource (cooked)
ox::gameplay::registerGameplayImporters(registry.importers()); // .oxbt behaviour trees, .oxanimctrl controllers
ox::assets::AssetManager manager(registry, &jobs);
ox::gameplay::AssetProviders providers(manager);
providers.registerIn(services);                            // all 7 interfaces + GameplayAssetEvents
```

- Names: UUID string, asset path (`Prefabs/enemy.oxprefab`), path without extension (`Prefabs/enemy`) or a unique
  file stem (`enemy`); `prefabNames()` lists paths without extension + unique stems.
- Loads are `loadSync` on first use (game thread), type-checked against the asset record, handles kept so used
  assets stay resident. Meshes use the import-time collision mesh when present, else LOD 0. Audio clips are decoded
  with `AudioEngine::loadSoundFromMemory`. Behaviour trees / animator controllers are `AssetType::Raw` blobs with
  info `kind` = `BehaviorTree` / `AnimatorController` (cookable without new asset types); controllers are
  `InlineAnimatorController` JSON built with `buildAnimatorController()`; their clip UUIDs are import dependencies.
- **Hot reload**: `AssetManager::onReloaded` -> caches invalidated -> `GameplayAssetEvents::changed`
  (`GameplayAssetChange{kind, id, path}`) -> queued by `Gameplay.Assets.HotReload` (PreUpdate -950) and applied at the
  next frame: scripts reload in place (`ScriptRuntime::reloadChangedScripts` -> `ScriptVM::reloadScript`: same
  instance and `self`, `on_reload`), behaviour trees are rebuilt keeping their blackboard
  (`AIRuntime::reloadBehaviorTree`), prefab instances are re-synchronised keeping overrides
  (`updatePrefabInstances`), animators using a changed skeleton/controller/clip are rebuilt
  (`AnimationRuntime::invalidateAssets`), mesh colliders using a changed mesh are recreated. Heightmap changes
  rebuild terrains (world section).

## World (`engine/gameplay/world/`, `OX_GAMEPLAY_HAS_WORLD=1` when the `world` module is configured)

`<oxwald/gameplay/world.hpp>`. `registerGameplayTypes()` also registers the world components
(`registerWorldGameplayTypes()`); `addGameplaySystems` calls `addWorldSystems(scheduler, services,
config.worldSystems)` unless `GameplayConfig::world = false` (`WorldSystemsConfig`: physics, vegetation, streaming,
bindLua, aspect, maxVegetationChunksPerFrame, streamingExecutor Auto/Inline/JobSystem/ThreadPool). Services:
`WorldRuntime` (all derived data keyed by entity: heightfields, quadtrees, splat maps, vegetation chunks, bodies,
TimeOfDay/WeatherController, chunk streamers) and `WorldRenderData`.

| Component (category "World") | Purpose |
| --- | --- |
| `Terrain` | source Procedural (noise + optional hydraulic/thermal erosion) / Heightmap asset (`IHeightmapProvider`) / Flat / External (streamed tiles); resolution, worldSize, heightScale/Offset, format, centred or offset placement; splat layer materials + auto-paint rules; CDLOD settings; collision -> static Jolt heightfield bodies per tile (play mode, user data = terrain entity so raycasts report it) |
| `Vegetation` | `world::VegetationLayer`s scattered around viewers (streaming sources + primary camera) in chunks, tree colliders, exclusion zones |
| `Sky` | Preetham turbidity, sun disc / moon / stars parameters |
| `TimeOfDay` | location, date, `localHours` (SaveGame), timeScale; drives the sun `Light` (rotation, colour, lux), `Environment` fog/ambient and the primary camera exposure; curve overrides; `WorldTimeEvent` (Sunrise/Sunset/Noon/Midnight) on `WorldRuntime::onTimeOfDayEvent` + EventBus |
| `Water` | Gerstner waves (explicit or `fromWind`), extent, fluid density, current |
| `Wind` | global wind + optional weather preset blending |
| `Buoyancy` | box sample points or explicit points, drag; forces/torques through `PhysicsRuntime` before the physics step |
| `StreamingSource` / `WorldStreaming` | viewers; chunk streaming of `world::ChunkData` files (`project://World/chunk_{x}_{z}.oxchunk` via Vfs) -> terrain tiles + vegetation, and/or prefabs (`Chunks/chunk_{x}_{z}` via `IPrefabProvider`); unload destroys spawned entities |

Systems: `Gameplay.World.Lifecycle` (PreUpdate -990), `.Streaming` (-500, play), `.Terrain` (-400), `.Environment`
(-300), `.Buoyancy` (FixedUpdate -10, play), `.Vegetation` (PostUpdate 300), `.Extract` (Extract 50). Visual data
(terrain build, LOD selection, sky) also runs in edit mode. Lua: components through `e:get("Terrain")`, plus the
`world` table (`terrainHeight`, `terrainNormal`, `waterHeight`, `timeOfDay`, `setTimeOfDay`, `isDay`,
`sunDirection`, `wind`, `setWeather`, `isAreaReady`, `time`).

**WorldRenderData** (renderer contract, filled in Extract on the game thread; read it in `IRenderer::extract()` and
copy): primary camera view/reversed-Z projection (`aspect` set by the renderer); per terrain the heightfield
(+version, dirty rect, fullUpload), splat map (+version, dirty rect), layer materials, shared `TerrainGridMesh`,
quadtree (re-select for shadow views), skirt depths, selected `TerrainPatchGpu`s; vegetation prototypes + batches of
`VegetationInstanceGpu` with cells; sky (`PreethamSky::Gpu`, sun/moon, stars rotation, atmosphere, main light);
water (`GerstnerParamsGpu`, transform, size); `WindGpu`; weather. See `world/render_data.hpp`.

Limits: brush edits are runtime-only (a component change rebuilds from the component); streamed vegetation needs a
heightfield tile in its chunk; the chunk `userData` blob is ignored; the JobSystem streaming executor is not covered
by tests (tests use Inline).

## Tests (`ox_gameplay_tests`, label `gameplay`)

Physics: body falls and lands on a static collider, child follows (cached world matrix), interpolation + teleport,
kinematic platform carries a body and collision events carry entities, scale change rebuilds the shape, component
removal -> static -> destroy, character controller walks, distance joint to the world, play-mode clone leaves the edit
world untouched. Scripts: trigger events reach Lua (`onTriggerEnter/Exit` on both sides), `entity.transform` helpers
(translate/lookAt), reflected field read/write incl. enums, nested arrays/structs, EntityRef, add/remove, sendEvent,
property overrides (+ runtime change, clamping), lifecycle incl. self-destroy + `onDestroy` with live components, prefab
spawn from Lua + raycast, module APIs (body/spline/animator/agent/audio, error count 0). Spline follower constant speed
+ orientation + events. Animator root motion (also rotated owner) + palette + events. Nav agent reaches a target
around a wall on a navmesh baked from box colliders; BT MoveTo -> Wait sequence; perception sight + wall LOS. Audio
source follows the transform (offline engine, inverse attenuation), physics occlusion. Debug draw in edit mode.
Network: prefab entity replicated server World -> client World over `MemoryNetwork`, Replicated field, smooth
interpolation (moves every frame at 30 Hz snapshots, ~100 ms behind), kinematic proxy body, despawn. Scene save/load
round trip (binary + JSON, byte-identical binary<->JSON) with all 20 gameplay components; prefab with gameplay
components instantiates twice and simulates. Async: Lua `await` on a C++ future, `physics.raycastAsync`,
`scene.nextFrame`; script coroutine started from C++; C++ entity coroutine cancelled (unwound) on destroy and on play
stop. Assets bridge: every provider kind from a temporary project (registry + manager), hot reload of a script,
behaviour tree and prefab into a running world (instance/self/blackboard/overrides kept). Prediction: latency
convergence, 20 % loss + server teleport replay, offline input. World: 14 tests (serialization, terrain physics,
heightmap reload, time of day, buoyancy, streaming files/prefabs, render data, Lua, play-in-editor clone).

Destruction (`destruction_tests.cpp`, 16 tests): component scene round trip (binary + JSON), shared deterministic
preparation, break on contact impulse over the threshold only (also a 34 m/s CCD cannonball vs a scaled column and a
dynamic crate), damage threshold/accumulation, inherited velocity, global + per-object debris budget, lifetime fade
and cleanup, structural collapse when anchors are knocked out, frozen chunk knocked out by a projectile, knocked-out
wall pieces leave along the shot, blast at the foot collapses a tower, beam on two columns (Static anchors) crumbles
over a broken column and falls with the second, replicated `broken`, Lua `destruction.*` + `onEvent("broken")`.

## Known limits / TODO

- Destruction: connectivity, not stress (a span held by one chunk does not break under its own weight); chunks are
  not re-fractured; outer UVs come from the fracture source (box/hull UVs for collider sources, not the render mesh's
  UVs); only the `broken` flag is replicated (later chunk releases and debris are local); scale changes after
  preparation are not picked up.
- Colliders on child entities are not merged into the parent's rigid body (use a Compound collider); dynamic bodies
  are expected to be roots or under non-moving parents (a moving parent reads as a teleport).
- Edit-mode raycasts/overlaps return nothing (bodies exist only in play mode).
- One navmesh surface per world; crowd agents driving a character controller are re-seated when they drift (no
  Detour position sync API); `NavObstacle` carving requires `dynamicObstacles` (tile cache, not serialisable).
- Networking: input replay covers character controllers (`PredictedCharacter`); other owned + predicted entities
  still use "local simulation, snapshot corrections above `correctionThreshold`". Rigid-body prediction is not
  rewound. Replicated property layout is fixed at spawn
  (components added later are not replicated); array/map fields are not replicable; replicated setters don't fire
  change signals.
- One script per entity (`ScriptComponent`); a `ScriptList` component would lift that.
- Perception LOS ignores bodies of entities that have a `Perception` component (agents don't block each other).
- Audio: one listener; occlusion counts bodies, not material thickness.
