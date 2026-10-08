# scene (`Oxwald::scene`)

ECS layer on EnTT. Deps: core, EnTT. Call `ox::registerSceneTypes()` once at startup (idempotent).

## World and entities (`world.hpp`)
```cpp
ox::World world;
ox::Entity car = world.create("Car");                 // Id, Name, Transform, Hierarchy, WorldTransform, Active
ox::Entity wheel = world.create("Wheel", car);        // child
wheel.setPosition({1, 0.3f, 1.5f});                   // marks the world transform dirty
auto& light = car.add<ox::LightComponent>();
if (auto* mr = car.tryGet<ox::MeshRendererComponent>()) { ... }
for (auto [e, l, wt] : world.view<ox::LightComponent, ox::WorldTransformComponent>().each()) { ... }
car.destroy();                                        // deferred until world.flushDestroyed() (end of frame)
ox::Entity same = world.find(uuid);                   // or world.resolve(entityRef)
```
- Hierarchy: `HierarchyComponent{parent, children}` (children vector keeps sibling order explicit and cheap to reorder).
  `setParent(parent, keepWorldTransform = true, index = -1)`, `children()`, `isAncestorOf`, cycle checks.
  `world.rootHandles()` keeps root order; `forEachInHierarchy(fn)` = pre-order.
- Transforms: `TransformComponent` (local pos/quat/scale). `Entity::transform()` returns a mutable ref and marks dirty;
  `registry.patch<TransformComponent>` also marks dirty (signal); writing through `registry.get<>` does **not**.
  `world.updateTransforms()` recomputes only dirty subtrees into `WorldTransformComponent::matrix`;
  `snapshotPreviousTransforms()` copies to `previous` (motion vectors). World-space: `worldMatrix()`, `worldPosition()`,
  `worldRotation()`, `setWorldTransform/Position/Rotation` (computed through the hierarchy, independent of the cache).
- `world.clone()` — deep copy (same entity ids/UUIDs) of all registered components; play-in-editor clones the edit world
  and drops the copy afterwards.

## Components (`components.hpp`, all reflected)
`Name`, `Id` (UUID), `Transform`, `Hierarchy` (runtime), `WorldTransform` (cached + previous), `Camera` (perspective/ortho,
fov in degrees, near/far (far ≤ 0 = infinite), aperture/shutter/ISO/exposure compensation, `primary`;
`projectionMatrix(aspect)` reversed-Z, `ev100()`, `exposure()`), `Light` (Directional/Point/Spot/AreaRect, color,
intensity in lux (directional) or lumens, range, inner/outer cone (deg), area size, castShadows, shadowResolution hint,
bias/normal bias, sourceRadius, volumetric + intensity), `MeshRenderer` (mesh + per-submesh material asset UUIDs,
shadow flags, visible, layerMask), `Environment` (skybox asset, sky intensity, sun `EntityRef`, ambient, fog),
`Tags`, `Active`, `PrefabInstance`, `SaveGame`, plus `UnknownComponents` (opaque data of unregistered types).
`EntityRef{Uuid}` is a reflected leaf (archive tag `entity`), remapped on paste/prefab instantiation.

## Component registry (`component_registry.hpp`)
Any module registers its (already reflected) components:
```cpp
OX_REFLECT_TYPE(RigidBodyComponent, "RigidBody").attributes(ox::attr::Category{"Physics"}).field(...);
ox::ComponentRegistry::instance().add<RigidBodyComponent>({.icon = "cube"});   // name = reflected name
```
`ComponentInfo`: name, type, category, icon, `removable`, `hiddenInInspector`, `serializable`, and type-erased
`has/add/remove/get/copy/notifyChanged`, `ref(world, e)` (a `reflect::ValueRef` for the inspector),
`serialize/deserialize`. After editing through a ValueRef call `notifyChanged` (fires entt update signals).

## Systems (`system.hpp`)
```cpp
class SpinSystem : public ox::ISystem {
    std::string_view name() const override { return "Spin"; }
    ox::SystemPhase phase() const override { return ox::SystemPhase::Update; }
    void update(ox::SystemContext& ctx) override { /* ctx.world, ctx.services, ctx.dt, ctx.fixedDt, ctx.alpha */ }
};
ox::SystemScheduler scheduler(1.0 / 60.0 /*fixed dt*/, 8 /*max fixed steps*/);
scheduler.emplace<ox::TransformSystem>();   // PostUpdate, order -1000
scheduler.emplace<SpinSystem>();
scheduler.attach(world, services);
scheduler.tick(world, services, frameDt);   // PreUpdate, FixedUpdate×N, Update, PostUpdate, Extract, flushDestroyed
```
`order()` sorts within a phase (ties: registration order); `playModeOnly()` systems are skipped unless
`setPlaying(true)`; `setEnabled(name, bool)`; `lastTimeMs(name)`.

## Serialization (`scene_serializer.hpp`)
`saveScene(world, "level.oxscene")` (binary) / `"level.oxscene.json"` (JSON), `loadScene(world, path)`,
`serializeWorld/deserializeWorld` (Document kind "scene"). Each entity: `{id, name, parent (entity ref), components:
{<name>: {...}}}` in hierarchy pre-order. Components of types not registered in the running build are kept as
`UnknownComponents` and written back unchanged (entity references inside are remapped too). Clipboard:
`copyEntities(world, selection)` → bytes, `pasteEntities(world, bytes, parent)` (new UUIDs, references inside the copied
set remapped, references outside kept). `SceneSerializeOptions` has entity/component/field filters (save games).

## Prefabs (`prefab.hpp`)
`createPrefab(world, root)` → Document (kind "prefab", prefab-local ids; by default links the source as an instance),
`instantiatePrefab(world, prefab, parent)`, overrides as property paths (`"Light.intensity"`, `"Tags"` for an added
component, `"-Light"` for a removed one, `"Name"`): `recordOverride`, `detectOverrides`, `recordDetectedOverrides`,
`revertOverride`, `revertAllOverrides`; `applyInstanceToPrefab` (new prefab version from an instance),
`updatePrefabInstances(world, prefab)` (propagates changes, keeps overrides, adds/removes entities). The root
Transform and name of an instance are always instance-specific.

## Limits / TODO
- Nested prefabs are flattened when a prefab is created from a subtree containing other instances.
- Override granularity is a component field (paths into vectors such as `position.x` override the whole field).
- Only entities created through `World::create*` take part in the hierarchy/UUID index.
- `World::clone()` copies only components registered in the `ComponentRegistry` (plus transform dirty / pending-destroy tags).
