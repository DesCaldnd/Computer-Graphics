#pragma once

#include <oxwald/core/result.hpp>
#include <oxwald/core/serial/convert.hpp>
#include <oxwald/core/serial/value.hpp>
#include <oxwald/scene/world.hpp>

#include <filesystem>
#include <functional>
#include <span>
#include <unordered_map>
#include <vector>

// Scene (de)serialization through the archive system.
//
// Document (kind "scene", version kSceneFormatVersion), root object "Scene":
//   entities: array<object "Entity"> in hierarchy pre-order, each
//       { id: uuid, name: string, parent: entity (nil for roots), components: map<object> }
// Component values are the reflected structs keyed by registered component name. Components whose type is
// not registered in this build are kept in UnknownComponents and written back unchanged.
namespace ox {

inline constexpr u32 kSceneFormatVersion = 1;

struct SceneSerializeOptions {
    serial::ConvertOptions convert;           // e.g. a field filter for save games
    std::function<bool(Entity)> entityFilter; // false skips the entity and its subtree
    // Optional per-entity component filter (component name); false skips the component.
    std::function<bool(Entity, std::string_view)> componentFilter;
};

struct InstantiateResult {
    std::vector<Entity> roots;                 // top-level entities created (sibling order)
    std::vector<Entity> entities;              // all created entities in file order
    std::unordered_map<Uuid, Uuid> idMap;      // file id -> world id
};

// Entities array value for the given subtree roots (all roots when `roots` is empty).
[[nodiscard]] serial::Value serializeEntities(const World& world, std::span<const Entity> roots,
                                              const SceneSerializeOptions& options = {});
// Creates entities from an entities array. regenerateIds: fresh UUIDs + entity references remapped (paste,
// prefab instancing); otherwise ids are kept (scene load). Top-level entities are attached to `parent`.
Result<InstantiateResult> instantiateEntities(World& world, const serial::Value& entities, bool regenerateIds,
                                              Entity parent = {});

[[nodiscard]] serial::Document serializeWorld(const World& world, const SceneSerializeOptions& options = {});
// Adds the document's entities to `world` (normally empty).
Status deserializeWorld(World& world, const serial::Document& doc);

// Binary .oxscene or JSON (.json suffix, e.g. "level.oxscene.json").
Status saveScene(const World& world, const std::filesystem::path& path, const SceneSerializeOptions& options = {});
Status loadScene(World& world, const std::filesystem::path& path);

// Clipboard: subtrees of the selection (descendants of selected entities are implied) as an OXB1 buffer.
[[nodiscard]] std::vector<std::byte> copyEntities(const World& world, std::span<const Entity> selection);
Result<std::vector<Entity>> pasteEntities(World& world, std::span<const std::byte> clipboard, Entity parent = {});

// Remaps Tag::EntityRef values (recursively) through idMap; unknown ids are left unchanged.
void remapEntityRefs(serial::Value& value, const std::unordered_map<Uuid, Uuid>& idMap);

} // namespace ox
