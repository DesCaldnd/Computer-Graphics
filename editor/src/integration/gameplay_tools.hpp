#pragma once

#include "core/common.hpp"
#include "viewport/viewport_renderer.hpp"

#include <oxwald/core/math.hpp>
#include <oxwald/core/serial/value.hpp>

#include <QString>

#include <optional>
#include <string>
#include <vector>

namespace ox {
class DebugDraw;
class Services;
class World;
} // namespace ox

// Editor-side helpers on top of the gameplay module (compiled to no-ops without it), so panels and the viewport
// stay free of gameplay includes: debug-draw toggles, navmesh baking, behaviour tree tracing, collider fitting,
// script property introspection, spline point editing and terrain sculpting.
namespace ox::editor {

class EditorContext;

[[nodiscard]] bool gameplayAvailable();

// ---- debug draw (viewport Show menu) ----
// Pushes the gameplay-related show flags into the runtimes (physics colliders/contacts, navmesh, splines, ...).
void applyGameplayDebugFlags(Services& services, const ShowFlags& flags);

// ---- navigation ----
// Bakes the NavMeshSurface of `surface` (nil = first surface of the edit world) as an undoable edit of its
// bakedData. Returns a status message; ok = false on failure.
struct BakeResult {
    bool ok = false;
    QString message;
};
BakeResult bakeNavMesh(EditorContext& ctx, const Uuid& surface = {});

// ---- behaviour trees ----
struct BtNodeRow {
    u32 id = 0;
    u32 parentId = 0;
    u32 depth = 0;
    QString name;
    QString type;
    int status = 0; // ai::BTStatus: 0 Idle, 1 Running, 2 Success, 3 Failure
    u64 lastTick = 0;
    bool tickedThisFrame = false;
};
struct BtSnapshot {
    bool hasComponent = false; // the entity has a BehaviorTree component
    bool running = false;      // a tree instance exists (play mode)
    u64 tickCount = 0;
    std::vector<BtNodeRow> nodes;
    std::vector<std::pair<QString, QString>> blackboard; // key -> value (text)
    QString source;            // asset name or "inline JSON"
};
[[nodiscard]] BtSnapshot behaviorTreeSnapshot(Services& services, World& world, const Uuid& entity);

// ---- physics ----
// Sets the entity's Collider to a box around its MeshRenderer bounds (local space). Undoable.
bool fitColliderToMesh(EditorContext& ctx, const UuidList& entities, QString* message = nullptr);

// ---- scripts ----
struct ScriptPropertyRow {
    std::string name;
    int type = 0;                // script::ScriptPropertyType
    serial::Value defaultValue;  // editor value (f64/i64/bool/string/vec2/3/4)
    serial::Value currentValue;  // override if present, else default
    bool overridden = false;
    std::optional<double> min, max;
    QString tooltip;
};
struct ScriptPropertiesResult {
    bool hasScript = false;
    QString scriptName;
    QString error;
    std::vector<ScriptPropertyRow> rows;
};
// Declared `properties = {...}` of the entity's Script (loaded in a sandboxed introspection VM, cached per file
// modification time) merged with the component's overrides.
[[nodiscard]] ScriptPropertiesResult scriptProperties(EditorContext& ctx, const Uuid& entity);
// Writes an override (Script.properties.<name>) for every entity; nullopt removes it (back to the default).
void setScriptPropertyOverride(EditorContext& ctx, const UuidList& entities, const std::string& name, int type,
                               const std::optional<serial::Value>& value, EditPhase phase = EditPhase::Single);

// ---- splines ----
[[nodiscard]] bool hasSpline(World& world, const Uuid& entity);
// World-space control points (empty without a Spline component).
[[nodiscard]] std::vector<glm::vec3> splinePointsWorld(World& world, const Uuid& entity);
void setSplinePointWorld(EditorContext& ctx, const Uuid& entity, int index, const glm::vec3& world, EditPhase phase);
// Inserts after `after` (-1 = append) at a world position; returns the new index.
int insertSplinePoint(EditorContext& ctx, const Uuid& entity, int after, const glm::vec3& world);
void removeSplinePoint(EditorContext& ctx, const Uuid& entity, int index);
void drawSplineHandles(DebugDraw& dd, World& world, const Uuid& entity, int selected, const glm::vec3& cameraPos);

// ---- terrain ----
enum class SculptOp { Raise, Lower, Smooth, Flatten };
struct SculptSettings {
    SculptOp op = SculptOp::Raise;
    float radius = 6.0f;
    float strength = 2.0f;
};
[[nodiscard]] bool hasTerrain(World& world, const Uuid& entity);
// Ray against the terrains of the world (WorldRuntime heights). Returns the hit point.
[[nodiscard]] std::optional<glm::vec3> raycastTerrain(Services& services, World& world, const Ray& ray, Uuid* terrain = nullptr);
// One brush application (dt-scaled). Runtime-only: terrain edits live in WorldRuntime (see docs).
bool sculptTerrain(Services& services, World& world, const Uuid& terrain, glm::vec2 xz, const SculptSettings& s, float dt);
// Coarse wireframe of the terrain (around `focus` within `radius`, finer near it) + brush circle.
void drawTerrainWire(DebugDraw& dd, Services& services, World& world, const Uuid& terrain,
                     std::optional<glm::vec3> brush, float brushRadius);

} // namespace ox::editor
