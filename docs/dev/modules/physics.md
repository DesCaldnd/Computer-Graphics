# physics — Jolt-backed rigid bodies, queries, characters

Target `Oxwald::physics` (`engine/physics`). Depends on `Oxwald::core` (types/log/assert) and glm publicly;
**Jolt is a private dependency**: no public header includes Jolt, so consumers never see `JPH_*` defines.
Umbrella header: `<oxwald/physics/physics.hpp>`.

## Jolt build configuration (vcpkg `joltphysics` 5.5.0, features: core)

| Setting | vcpkg build | Consequence |
| --- | --- | --- |
| `JPH_DOUBLE_PRECISION` | off | single precision, `RVec3 == Vec3`; keep worlds within ~±10 km of the origin |
| `JPH_CROSS_PLATFORM_DETERMINISTIC` | off | deterministic per binary/platform only (see Determinism) |
| `JPH_DEBUG_RENDERER` | off (no `debugrenderer` feature) | `JPH::DebugRenderer` unavailable → we draw colliders ourselves |
| `JPH_PROFILE_ENABLED` | off (no `profiler` feature) | no Jolt zones in Tracy |
| `JPH_OBJECT_STREAM` | **on** (exported compile definition) | propagated privately through `Jolt::Jolt` |
| RTTI | off (`-fno-rtti`, no typeinfo in `libJolt.a`) | `ox_physics` sources are compiled with `-fno-rtti` |
| Asserts | debug lib only (`JPH_DEBUG` → `JPH_ENABLE_ASSERTS`) | RelWithDebInfo/MinSizeRel are mapped to the release lib |
| ObjectLayer | 16-bit | we use ≤ 32 object layers |

`detail::ensureJoltInitialized()` asserts `JPH::VerifyJoltVersionID()` once per process, so a feature-define mismatch
(the classic source of mysterious Jolt crashes) fails loudly at start-up. `ox::physics::backendInfo()` returns e.g.
`Jolt 5.5.0: Single precision ARM 64-bit with instructions: NEON (16-bit ObjectLayer) (ObjectStream) (C++ Exceptions)`.

## API overview

| Header | Contents |
| --- | --- |
| `types.hpp` | `BodyHandle` (wraps BodyID index+sequence), `ConstraintHandle`, `CharacterHandle` (generational), `ObjectLayer`, `layers::*`, `LayerMask`, `MotionType`, `Transform`, `Aabb` |
| `collision_layers.hpp` | `CollisionLayers`: named layers (Static, Dynamic, Kinematic, Character, Trigger, Debris + user layers), symmetric collision matrix, object→broad-phase mapping |
| `shape.hpp` | `ShapeDesc` (box, sphere, capsule, cylinder, convex hull, triangle mesh, height field, compound; `scale`, `centerOfMassOffset`), `ShapeRef` (ref-counted, shareable), `createShape`, `makeScaled/RotatedTranslated/OffsetCenterOfMass`, `ShapeCache` (dedupe by description hash + equality) |
| `body.hpp` | `BodyDesc` (motion type, layer, sensor, mass/inertia override, friction, restitution, damping, gravity factor, CCD, sleeping, lock axes, user data) |
| `physics_world.hpp` | `PhysicsWorldDesc`, `PhysicsWorld` (bodies, events, queries, characters, constraints, debug draw, snapshot) |
| `events.hpp` | `ContactEvent` (Begin/Persist/End, points, normal, estimated impulse), `TriggerEvent` (Enter/Stay/Exit), `ConstraintBrokenEvent` |
| `query.hpp` | `QueryFilter` (layer mask, ignore list, sensors, predicate), `RayHit`, `ShapeCastHit`, `ClosestPointResult` |
| `character.hpp` | `CharacterDesc`, `CharacterMoveInput`, `CharacterState`, `GroundState` |
| `constraint.hpp` | `ConstraintDesc` (Fixed, Point, Hinge, Slider, Distance, Cone; limits, motors, break thresholds) |
| `debug_draw.hpp` | `PhysicsDebugSink` (`line`, `triangle`, `text`), `DebugDrawOptions` |
| `job_executor.hpp` | `IPhysicsJobExecutor` — run Jolt jobs on the engine job system |
| `interpolation.hpp` | `FixedStepper` (accumulator), `TransformInterpolator` (prev/current + lerp/slerp) |

### Threading
All `PhysicsWorld` methods are called from one thread (the fixed-update thread), never concurrently with `step()`.
Jolt parallelises inside `step()` — on its own `JobSystemThreadPool` (`PhysicsWorldDesc::workerThreads`) or on an
`IPhysicsJobExecutor` (adapted to `JPH::JobSystemWithBarrier`). Contact callbacks from Jolt worker threads are only
buffered (mutex); they are sorted and turned into events on the calling thread after the step.

### Create a world and bodies
```cpp
#include <oxwald/physics/physics.hpp>
using namespace ox::physics;

PhysicsWorldDesc desc;
ObjectLayer props = *desc.layers.addLayer("Props");          // user layer, collides with Static/Dynamic/...
desc.layers.setCollides(props, layers::Debris, false);
PhysicsWorld world(desc);                                       // register as a service in ox::Services later

BodyDesc floor;
floor.shape = world.shapeCache().getOrCreate(ShapeDesc::box({50.f, 0.5f, 50.f}));
floor.position = {0.f, -0.5f, 0.f};
floor.motionType = MotionType::Static;
world.createBody(floor);

BodyDesc crate;
crate.shape = world.shapeCache().getOrCreate(ShapeDesc::box(glm::vec3(0.5f)));
crate.position = {0.f, 5.f, 0.f};
crate.rotation = glm::angleAxis(0.3f, glm::vec3(0, 1, 0));    // quaternions only
crate.mass = 20.f;
crate.restitution = 0.2f;
crate.ccd = true;
crate.userData = entityId;                                       // u64 → entity
BodyHandle body = world.createBody(crate);

world.addImpulse(body, {0.f, 0.f, 50.f});
world.step(1.f / 60.f);
Transform t = world.getTransform(body);
```
Compound / hull / mesh / terrain:
```cpp
ShapeRef dumbbell = createShape(ShapeDesc::compound({
    {ShapeDesc::sphere(0.3f), {-0.6f, 0, 0}},
    {ShapeDesc::sphere(0.3f), { 0.6f, 0, 0}},
    {ShapeDesc::box({0.6f, 0.08f, 0.08f}), {}},
}));
ShapeRef rock    = createShape(ShapeDesc::convexHull(points));
ShapeRef level   = createShape(ShapeDesc::triangleMesh(vertices, indices));        // static/kinematic
ShapeRef terrain = createShape(ShapeDesc::heightField(heights, 257, {-128, 0, -128}, {1, 1, 1}));
```

### Fixed step + render interpolation
```cpp
FixedStepper stepper(60.f);
TransformInterpolator interp;            // interp.track(body) for every rendered dynamic body
// per frame:
for (u32 i = stepper.advance(frameDt); i > 0; --i) {
    world.step(stepper.fixedDt());
    interp.capture(world);
}
Transform renderXf = interp.get(body, stepper.alpha());
```

### Raycasts and other queries
```cpp
QueryFilter filter;
filter.layerMask = kAllLayers & ~layerBit(layers::Trigger);
BodyHandle self[] = {playerBody};
filter.ignoreBodies = self;
if (auto hit = world.raycast(eye, forward, 100.f, filter)) {
    // hit->body, hit->userData, hit->point, hit->normal, hit->distance
}
auto hits   = world.raycastAll(eye, forward, 100.f);            // sorted by distance
auto sweep  = world.sphereCast(pos, 0.3f, dir, 5.f);              // also boxCast/capsuleCast/shapeCast
auto nearby = world.overlapSphere(center, 4.f);                    // also overlapBox/overlapShape/overlapAabb
auto close  = world.closestPoint(p, 2.f);                          // body + surface point + distance
```

### Triggers and contact events
```cpp
BodyDesc zone;
zone.shape = createShape(ShapeDesc::box({2, 1, 2}));
zone.motionType = MotionType::Static;
zone.isSensor = true;                                  // layer defaults to layers::Trigger
world.createBody(zone);

world.setTriggerCallback([](const TriggerEvent& e) {
    if (e.type == TriggerEventType::Enter) { /* e.trigger, e.other, e.otherUserData */ }
});
world.step(dt);
for (const ContactEvent& c : world.contactEvents()) {   // or setContactCallback
    if (c.type == ContactEventType::Begin && c.normalImpulse > 500.f) playImpactSound(c.points[0]);
}
```
Semantics: events are per body pair (compound sub-shapes merged), `bodyA.id < bodyB.id`, normal points A→B. Begin
carries an impulse estimate (`JPH::EstimateCollisionResponse`; the contact listener runs before the solver, so the
real solved impulse is not available). Persist is emitted once per step per pair (disable with
`PhysicsWorldDesc::reportPersistContacts = false`), Stay every step for overlapping triggers. Jolt drops contacts of
bodies that fall asleep — the module keeps them "frozen" so a resting object does **not** produce End/Exit; destroying
a body emits End/Exit on the next step. `BodyDesc::reportContacts = false` silences a body's contacts.

### Character controller
```cpp
CharacterDesc cd;
cd.position = spawn;                        // feet
cd.height = 1.8f; cd.radius = 0.3f;
cd.maxSlopeAngle = glm::radians(45.f);
cd.maxStepHeight = 0.35f;                   // auto stairs
cd.userData = entityId;
CharacterHandle ch = world.createCharacter(cd);

// every fixed step, before world.step():
world.moveCharacter(ch, dt, CharacterMoveInput{.desiredVelocity = wishDir * 4.f, .jump = jumpPressed});
CharacterState s = world.getCharacterState(ch);   // position, groundState, groundNormal, groundBody, ...
```
`moveCharacter` implements the usual logic: on ground → inherit the ground (platform) velocity + input (+ jump),
in air → keep vertical speed with `airControl`, plus gravity; then `CharacterVirtual::ExtendedUpdate` (collide &
slide, stair step-up, stick to floor). For custom movement use `setCharacterVelocity` + `updateCharacter`.
By default a kinematic inner body (layer `Character`) makes the character visible to raycasts, triggers and
dynamic bodies; dynamic bodies are pushed with `maxStrength`. Crouch via `setCharacterShape`.

### Constraints
```cpp
ConstraintDesc door;
door.type = ConstraintType::Hinge;
door.bodyA = doorBody;                       // bodyB empty → attached to the world
door.pointA = hingeWorldPos;
door.axis = {0, 1, 0};
door.limitsEnabled = true; door.limitMin = -1.5f; door.limitMax = 0.f;
door.breakTorque = 5000.f;                   // breakable → ConstraintBrokenEvent
ConstraintHandle h = world.createConstraint(door);
world.setMotor(h, MotorMode::Velocity, 1.f);
f32 angle = world.getJointValue(h);          // motion of bodyA relative to bodyB/world
```
All points/axes are world space at creation. Destroying a body destroys its constraints.

### Debug draw
```cpp
struct Sink : PhysicsDebugSink {
    void line(const glm::vec3& a, const glm::vec3& b, const Color& c) override { debugDraw.line(a, b, c); }
};
DebugDrawOptions o; o.aabbs = true; o.contacts = true; o.velocities = true;
world.debugDraw(sink, o);
```
Wireframes are generated from shape data (box, sphere, capsule, cylinder, convex hull faces; meshes, height fields
and other shapes through Jolt's triangle iterator, capped by `maxTrianglesPerBody`). Compound/scaled/rotated/COM-offset
shapes are flattened with `CollectTransformedShapes`. Colors distinguish static/dynamic/kinematic/sensor/sleeping.

### Snapshot, determinism
```cpp
std::vector<u8> snap = world.saveState();    // bodies, velocities, contact cache, constraints, characters, event tracking
...
world.restoreState(snap);                     // same objects must exist (it does not create/destroy)
```
Restore → re-simulate reproduces bit-identical transforms and the same events (tested). Jolt is deterministic for
the same binary, same API call order and same inputs, independent of thread count. The vcpkg build is **not**
`JPH_CROSS_PLATFORM_DETERMINISTIC`, so lock-step networking across different CPUs/compilers must not rely on bitwise
equality (use server authority / state correction, or rebuild Jolt with the define — engine and lib must match).
Snapshots embed `JPH_VERSION_ID` and are rejected by a differently-configured build. Bodies should be created in the
same order on all peers (body IDs are part of the state).

## Known limits / TODO
- No soft bodies, vehicles, ragdolls (Jolt supports them; add when gameplay needs them).
- Jolt's `ContactListener::OnContactValidate` (one-way platforms) and collision groups (no collision between jointed
  bodies) are not exposed yet.
- `CharacterVirtual` contact callbacks (per-character hit events) are not exposed; ground info is.
- Height fields: `kHeightFieldHole` marks holes; per-triangle materials not exposed. Mesh shapes have no materials.
- Debug draw uses its own sink; adapt to core `ox::DebugDraw` once it lands.
- Profiling: wrap `step()` in `OX_PROFILE_ZONE` once core profiling is available (Jolt's own profiler is not built).
- ECS binding (components + `FixedUpdate` system) belongs to `gameplay`/integration.
