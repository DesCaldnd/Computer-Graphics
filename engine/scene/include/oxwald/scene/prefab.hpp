#pragma once

#include <oxwald/core/result.hpp>
#include <oxwald/core/serial/value.hpp>
#include <oxwald/scene/world.hpp>

#include <string>
#include <string_view>
#include <vector>

// Prefabs. A prefab asset is a Document (kind "prefab", root "Prefab" {id, entities}) whose entity list has the
// prefab root first; entity ids inside are prefab-local. Instances carry PrefabInstanceComponent on every
// entity (prefab id + the prefab-local source id). Per-instance overrides are property paths
// "Component.field[.sub]" (or "Component" for a component added on the instance, "Name" for the entity name)
// stored on the overriding entity; they survive updatePrefabInstances(). The instance root's Transform and name
// are always instance-specific.
//
// Limits: nested prefabs are flattened (an instance inside a prefab becomes plain entities of the outer prefab).
namespace ox {

inline constexpr u32 kPrefabFormatVersion = 1;

struct CreatePrefabOptions {
    Uuid prefabId;            // nil: generate (entity ids inside the prefab derive from it deterministically)
    bool linkSource = true;   // turn the source subtree into an instance of the new prefab
};

[[nodiscard]] serial::Document createPrefab(World& world, Entity root, const CreatePrefabOptions& options = {});
[[nodiscard]] Uuid prefabId(const serial::Document& prefab);

// New entities with fresh UUIDs (entity references remapped); returns the instance root.
Result<Entity> instantiatePrefab(World& world, const serial::Document& prefab, Entity parent = {});

// Root of the prefab instance containing `e` (invalid if e is not part of an instance).
[[nodiscard]] Entity prefabInstanceRoot(Entity e);

void recordOverride(Entity e, std::string_view propertyPath);
[[nodiscard]] bool isOverridden(Entity e, std::string_view propertyPath);
// Fields that differ from the prefab, as property paths (top-level component fields, whole added components).
[[nodiscard]] std::vector<std::string> detectOverrides(Entity e, const serial::Document& prefab);
// Records every detected difference of the instance subtree as an override. Returns the number found.
usize recordDetectedOverrides(Entity instanceRoot, const serial::Document& prefab);
// Restores the prefab value of one property and drops the override.
bool revertOverride(Entity e, std::string_view propertyPath, const serial::Document& prefab);
void revertAllOverrides(Entity instanceRoot, const serial::Document& prefab);

// Writes the instance state back into a new version of the prefab (same id) and clears its overrides.
[[nodiscard]] serial::Document applyInstanceToPrefab(World& world, Entity instanceRoot, const serial::Document& prefab);

// Re-synchronises every instance of the prefab in `world` with its (changed) contents, keeping overrides.
// Entities added to the prefab are created, removed ones destroyed. Returns the number of instances updated.
usize updatePrefabInstances(World& world, const serial::Document& prefab);

} // namespace ox
