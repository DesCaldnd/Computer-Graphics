#include <oxwald/core/profile.hpp>
#include <oxwald/core/serial/format.hpp>
#include <oxwald/scene/component_registry.hpp>
#include <oxwald/scene/scene_serializer.hpp>

#include <algorithm>
#include <unordered_set>

namespace ox {

using serial::Tag;
using serial::TypeDesc;
using serial::Value;

void remapEntityRefs(Value& value, const std::unordered_map<Uuid, Uuid>& idMap) {
    value.visitMutable([&](Value& v) {
        if (v.tag() != Tag::EntityRef) return;
        auto it = idMap.find(v.getUuid());
        if (it != idMap.end()) v = Value::makeEntityRef(it->second);
    });
}

namespace {

Value serializeEntity(const World& world, entt::entity e, bool isTop, const SceneSerializeOptions& options) {
    World& w = const_cast<World&>(world);
    const Entity entity{e, &w};
    Value obj = Value::makeObject("Entity");
    obj.set("id", Value::makeUuid(entity.uuid()));
    obj.set("name", Value::makeString(entity.name()));
    const Entity parent = entity.parent();
    obj.set("parent", Value::makeEntityRef(!isTop && parent.valid() ? parent.uuid() : Uuid{}));

    Value comps = Value::makeMap(TypeDesc::of(Tag::Object));
    for (const ComponentInfo* info : ComponentRegistry::instance().all()) {
        if (!info->serializable || !info->has(world, e)) continue;
        if (options.componentFilter && !options.componentFilter(entity, info->name)) continue;
        comps.fields().emplace_back(info->name, info->serialize(world, e, options.convert));
    }
    if (const auto* unknown = world.registry().try_get<UnknownComponents>(e)) {
        for (const auto& [name, v] : unknown->entries) {
            if (comps.find(name)) continue;
            if (options.componentFilter && !options.componentFilter(entity, name)) continue;
            comps.fields().emplace_back(name, v);
        }
    }
    obj.set("components", std::move(comps));
    return obj;
}

void serializeSubtree(const World& world, entt::entity e, bool isTop, const SceneSerializeOptions& options, Value& out) {
    World& w = const_cast<World&>(world);
    if (options.entityFilter && !options.entityFilter(Entity{e, &w})) return;
    out.push(serializeEntity(world, e, isTop, options));
    if (const auto* h = world.registry().try_get<HierarchyComponent>(e)) {
        for (auto c : h->children) serializeSubtree(world, c, false, options, out);
    }
}

} // namespace

Value serializeEntities(const World& world, std::span<const Entity> roots, const SceneSerializeOptions& options) {
    OX_PROFILE_ZONE();
    Value arr = Value::makeArray(TypeDesc::of(Tag::Object));
    if (roots.empty()) {
        for (auto e : world.rootHandles()) serializeSubtree(world, e, true, options, arr);
    } else {
        for (const Entity& r : roots) {
            if (r.valid()) serializeSubtree(world, r.handle(), true, options, arr);
        }
    }
    return arr;
}

Result<InstantiateResult> instantiateEntities(World& world, const Value& entities, bool regenerateIds, Entity parent) {
    OX_PROFILE_ZONE();
    if (!entities.isArray()) return makeError("scene: 'entities' is not an array");
    InstantiateResult result;

    // Pass 1: ids.
    std::vector<const Value*> list;
    for (const auto& v : entities.items()) {
        if (!v.isObject()) return makeError("scene: entity entry is not an object");
        const Value* id = v.find("id");
        const Uuid fileId = id ? id->getUuid() : Uuid{};
        const Uuid worldId = (regenerateIds || !fileId.isValid()) ? Uuid::generate() : fileId;
        if (fileId.isValid()) {
            if (result.idMap.contains(fileId)) return makeError("scene: duplicate entity id {}", fileId.toString());
            result.idMap.emplace(fileId, worldId);
        }
        if (!regenerateIds && world.find(worldId).valid()) {
            return makeError("scene: entity {} already exists in the world", worldId.toString());
        }
        list.push_back(&v);
    }

    // Pass 2: create entities.
    std::vector<Value> remapped;
    if (regenerateIds) {
        remapped.reserve(list.size());
        for (const Value* v : list) {
            remapped.push_back(*v);
            remapEntityRefs(remapped.back(), result.idMap);
        }
    }
    for (usize i = 0; i < list.size(); ++i) {
        const Value& v = regenerateIds ? remapped[i] : *list[i];
        const Value* id = list[i]->find("id");
        const Value* name = v.find("name");
        const Uuid fileId = id ? id->getUuid() : Uuid{};
        const Uuid worldId = fileId.isValid() ? result.idMap.at(fileId) : Uuid::generate();
        result.entities.push_back(world.createWithId(worldId, name ? name->getString() : std::string("Entity")));
    }

    // Pass 3: hierarchy (file order keeps sibling order) and components.
    const auto& registry = ComponentRegistry::instance();
    for (usize i = 0; i < list.size(); ++i) {
        const Value& v = regenerateIds ? remapped[i] : *list[i];
        Entity e = result.entities[i];
        const Value* parentRef = v.find("parent");
        Entity p;
        if (parentRef && parentRef->getUuid().isValid()) {
            p = world.find(parentRef->getUuid());
            // A parent reference that is neither in the set nor in the world is treated as a root.
        }
        const bool top = !p.valid();
        if (top) p = parent;
        if (p.valid()) world.setParent(e.handle(), p.handle(), false);
        if (top) result.roots.push_back(e);

        const Value* comps = v.find("components");
        if (!comps) continue;
        for (const auto& [compName, compValue] : comps->fields()) {
            if (const ComponentInfo* info = registry.find(compName)) {
                if (!info->deserialize(world, e.handle(), compValue)) {
                    OX_LOG_WARN("scene", "entity '{}': component {} has incompatible data", e.name(), compName);
                }
            } else {
                auto& unknown = world.registry().get_or_emplace<UnknownComponents>(e.handle());
                unknown.entries.emplace_back(compName, compValue);
            }
        }
        world.markTransformDirty(e.handle());
    }
    world.updateTransforms();
    world.snapshotPreviousTransforms();
    return result;
}

serial::Document serializeWorld(const World& world, const SceneSerializeOptions& options) {
    serial::Document doc;
    doc.kind = "scene";
    doc.version = kSceneFormatVersion;
    doc.root = Value::makeObject("Scene");
    doc.root.set("entities", serializeEntities(world, {}, options));
    return doc;
}

Status deserializeWorld(World& world, const serial::Document& doc) {
    if (!doc.kind.empty() && doc.kind != "scene" && doc.kind != "prefab" && doc.kind != "entities") {
        return makeError("document kind '{}' is not a scene", doc.kind);
    }
    if (doc.version > kSceneFormatVersion) {
        OX_LOG_WARN("scene", "scene format version {} is newer than supported {}; loading what is understood",
                    doc.version, kSceneFormatVersion);
    }
    const Value* entities = doc.root.find("entities");
    if (!entities) return makeError("scene document has no 'entities'");
    auto r = instantiateEntities(world, *entities, false);
    if (!r) return r.error();
    return {};
}

Status saveScene(const World& world, const std::filesystem::path& path, const SceneSerializeOptions& options) {
    return serial::saveDocument(path, serializeWorld(world, options), serial::formatForPath(path));
}

Status loadScene(World& world, const std::filesystem::path& path) {
    auto doc = serial::loadDocument(path);
    if (!doc) return doc.error();
    return deserializeWorld(world, *doc);
}

std::vector<std::byte> copyEntities(const World& world, std::span<const Entity> selection) {
    // Keep only selection tops (an entity whose ancestor is also selected is copied with that ancestor),
    // ordered as they appear in the hierarchy.
    std::unordered_set<entt::entity> selected;
    for (const Entity& e : selection) {
        if (e.valid()) selected.insert(e.handle());
    }
    std::vector<Entity> tops;
    World& w = const_cast<World&>(world);
    world.forEachInHierarchy([&](entt::entity e) {
        if (!selected.contains(e)) return;
        for (Entity p = Entity{e, &w}.parent(); p.valid(); p = p.parent()) {
            if (selected.contains(p.handle())) return;
        }
        tops.emplace_back(e, &w);
    });
    serial::Document doc;
    doc.kind = "entities";
    doc.version = kSceneFormatVersion;
    doc.root = Value::makeObject("Clipboard");
    doc.root.set("entities", serializeEntities(world, tops));
    return serial::encodeBinary(doc);
}

Result<std::vector<Entity>> pasteEntities(World& world, std::span<const std::byte> clipboard, Entity parent) {
    auto doc = serial::decodeAny(clipboard);
    if (!doc) return doc.error();
    const Value* entities = doc->root.find("entities");
    if (!entities) return makeError("clipboard has no entities");
    auto r = instantiateEntities(world, *entities, true, parent);
    if (!r) return r.error();
    return std::move(r->roots);
}

} // namespace ox
