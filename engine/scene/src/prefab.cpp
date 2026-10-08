#include <oxwald/scene/component_registry.hpp>
#include <oxwald/scene/prefab.hpp>
#include <oxwald/scene/scene_serializer.hpp>

#include <algorithm>
#include <unordered_map>

namespace ox {

using serial::Tag;
using serial::Value;

namespace {

constexpr std::string_view kPrefabComponent = "PrefabInstance";

std::vector<Entity> subtreeOf(Entity root) {
    std::vector<Entity> out;
    std::vector<Entity> stack{root};
    while (!stack.empty()) {
        Entity e = stack.back();
        stack.pop_back();
        out.push_back(e);
        auto children = e.children();
        stack.insert(stack.end(), children.rbegin(), children.rend());
    }
    return out;
}

SceneSerializeOptions withoutPrefabLinks() {
    SceneSerializeOptions o;
    o.componentFilter = [](Entity, std::string_view name) { return name != kPrefabComponent; };
    return o;
}

// Rewrites entity ids ("id" uuid fields) and entity references through idMap.
void remapEntityList(Value& entities, const std::unordered_map<Uuid, Uuid>& idMap) {
    remapEntityRefs(entities, idMap);
    for (auto& ev : entities.items()) {
        if (Value* id = ev.find("id")) {
            if (auto it = idMap.find(id->getUuid()); it != idMap.end()) *id = Value::makeUuid(it->second);
        }
    }
}

struct PrefabView {
    Uuid id;
    Uuid rootSource;
    std::vector<const Value*> entities; // file order
    std::unordered_map<Uuid, const Value*> bySource;
};

PrefabView viewOf(const serial::Document& prefab) {
    PrefabView v;
    if (const Value* id = prefab.root.find("id")) v.id = id->getUuid();
    if (const Value* list = prefab.root.find("entities"); list && list->isArray() && !list->isPacked()) {
        for (const auto& ev : list->items()) {
            const Value* id = ev.find("id");
            if (!id) continue;
            const Uuid source = id->getUuid();
            if (v.entities.empty()) v.rootSource = source;
            v.entities.push_back(&ev);
            v.bySource.emplace(source, &ev);
        }
    }
    return v;
}

// source id -> instance entity, for entities of this prefab within the instance subtree.
std::unordered_map<Uuid, Entity> instanceMap(Entity root, Uuid prefab) {
    std::unordered_map<Uuid, Entity> map;
    for (Entity e : subtreeOf(root)) {
        const auto* pic = e.tryGet<PrefabInstanceComponent>();
        if (pic && pic->prefab == prefab) map.emplace(pic->sourceId, e);
    }
    return map;
}

std::unordered_map<Uuid, Uuid> sourceToInstanceIds(const std::unordered_map<Uuid, Entity>& map) {
    std::unordered_map<Uuid, Uuid> ids;
    for (const auto& [source, e] : map) ids.emplace(source, e.uuid());
    return ids;
}

std::vector<std::string_view> splitPath(std::string_view path) {
    std::vector<std::string_view> parts;
    usize start = 0;
    while (start <= path.size()) {
        const usize dot = path.find('.', start);
        const usize end = dot == std::string_view::npos ? path.size() : dot;
        if (end > start) parts.push_back(path.substr(start, end - start));
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    return parts;
}

const Value* valueAt(const Value& root, std::span<const std::string_view> segments) {
    const Value* cur = &root;
    for (auto seg : segments) {
        if (!cur->isObject() && !cur->isMap()) return cur; // leaf reached (e.g. "position.x" -> position)
        cur = cur->find(seg);
        if (!cur) return nullptr;
    }
    return cur;
}

// Copies the sub-value at `segments` from src into dst (creating nothing that src lacks).
void copyAt(Value& dst, const Value& src, std::span<const std::string_view> segments) {
    if (segments.empty()) {
        dst = src;
        return;
    }
    if ((!dst.isObject() && !dst.isMap()) || (!src.isObject() && !src.isMap())) {
        dst = src; // path goes into a leaf: the whole leaf is the override unit
        return;
    }
    const Value* s = src.find(segments.front());
    if (!s) {
        dst.erase(segments.front());
        return;
    }
    Value* d = dst.find(segments.front());
    if (!d) {
        dst.set(segments.front(), *s);
        return;
    }
    copyAt(*d, *s, segments.subspan(1));
}

const Value* componentsOf(const Value& entityValue) { return entityValue.find("components"); }

Value remappedEntity(const Value& prefabEntity, const std::unordered_map<Uuid, Uuid>& ids) {
    Value copy = prefabEntity;
    remapEntityRefs(copy, ids);
    return copy;
}

void setUnknown(Entity e, const std::string& name, const Value& v) {
    auto& unknown = e.world()->registry().get_or_emplace<UnknownComponents>(e.handle());
    for (auto& [n, existing] : unknown.entries) {
        if (n == name) {
            existing = v;
            return;
        }
    }
    unknown.entries.emplace_back(name, v);
}

// Restores one component (or a field path inside it) from the prefab value.
void restoreComponent(Entity e, const std::string& compName, const Value& prefabValue,
                      std::span<const std::string_view> fieldPath) {
    World& world = *e.world();
    const ComponentInfo* info = ComponentRegistry::instance().find(compName);
    if (!info) {
        setUnknown(e, compName, prefabValue);
        return;
    }
    if (fieldPath.empty() || !info->has(world, e.handle())) {
        info->deserialize(world, e.handle(), prefabValue);
        return;
    }
    Value cur = info->serialize(world, e.handle());
    copyAt(cur, prefabValue, fieldPath);
    info->deserialize(world, e.handle(), cur);
}

} // namespace

Uuid prefabId(const serial::Document& prefab) {
    const Value* id = prefab.root.find("id");
    return id ? id->getUuid() : Uuid{};
}

serial::Document createPrefab(World& world, Entity root, const CreatePrefabOptions& options) {
    OX_ASSERT(root.valid(), "createPrefab: invalid root");
    const Uuid id = options.prefabId.isValid() ? options.prefabId : Uuid::generate();
    const auto subtree = subtreeOf(root);
    std::unordered_map<Uuid, Uuid> toLocal;
    for (Entity e : subtree) toLocal.emplace(e.uuid(), Uuid::generate());

    const Entity roots[] = {root};
    Value entities = serializeEntities(world, roots, withoutPrefabLinks());
    remapEntityList(entities, toLocal);

    serial::Document doc;
    doc.kind = "prefab";
    doc.version = kPrefabFormatVersion;
    doc.root = Value::makeObject("Prefab");
    doc.root.set("id", Value::makeUuid(id));
    doc.root.set("entities", std::move(entities));

    if (options.linkSource) {
        for (Entity e : subtree) {
            e.addOrReplace<PrefabInstanceComponent>(PrefabInstanceComponent{id, toLocal.at(e.uuid()), e == root, {}});
        }
    }
    return doc;
}

Result<Entity> instantiatePrefab(World& world, const serial::Document& prefab, Entity parent) {
    if (prefab.kind != "prefab") return makeError("document kind '{}' is not a prefab", prefab.kind);
    const PrefabView view = viewOf(prefab);
    if (view.entities.empty()) return makeError("prefab has no entities");
    auto result = instantiateEntities(world, *prefab.root.find("entities"), true, parent);
    if (!result) return result.error();
    OX_ASSERT(result->entities.size() == view.entities.size(), "prefab instantiation size mismatch");
    for (usize i = 0; i < view.entities.size(); ++i) {
        const Uuid source = view.entities[i]->find("id")->getUuid();
        result->entities[i].addOrReplace<PrefabInstanceComponent>(PrefabInstanceComponent{view.id, source, i == 0, {}});
    }
    return result->entities.front();
}

Entity prefabInstanceRoot(Entity e) {
    const auto* pic = e.tryGet<PrefabInstanceComponent>();
    if (!pic) return {};
    for (Entity cur = e; cur.valid(); cur = cur.parent()) {
        const auto* p = cur.tryGet<PrefabInstanceComponent>();
        if (!p || p->prefab != pic->prefab) break;
        if (p->isRoot) return cur;
    }
    return {};
}

void recordOverride(Entity e, std::string_view propertyPath) {
    auto* pic = e.tryGet<PrefabInstanceComponent>();
    if (!pic) return;
    // A path is covered by an already recorded parent path ("Light" covers "Light.color").
    for (const auto& existing : pic->overrides) {
        if (propertyPath == existing ||
            (propertyPath.starts_with(existing) && propertyPath.size() > existing.size() && propertyPath[existing.size()] == '.')) {
            return;
        }
    }
    const std::string prefix = std::string(propertyPath) + ".";
    std::erase_if(pic->overrides, [&](const std::string& o) { return o.starts_with(prefix); });
    pic->overrides.emplace_back(propertyPath);
}

bool isOverridden(Entity e, std::string_view propertyPath) {
    const auto* pic = e.tryGet<PrefabInstanceComponent>();
    if (!pic) return false;
    for (const auto& o : pic->overrides) {
        if (o == propertyPath) return true;
        if (propertyPath.starts_with(o) && propertyPath.size() > o.size() && propertyPath[o.size()] == '.') return true;
    }
    return false;
}

std::vector<std::string> detectOverrides(Entity e, const serial::Document& prefab) {
    std::vector<std::string> out;
    const auto* pic = e.tryGet<PrefabInstanceComponent>();
    const PrefabView view = viewOf(prefab);
    if (!pic || pic->prefab != view.id) return out;
    auto it = view.bySource.find(pic->sourceId);
    if (it == view.bySource.end()) return out;
    const Entity root = prefabInstanceRoot(e);
    const Value pe = remappedEntity(*it->second, sourceToInstanceIds(instanceMap(root, view.id)));
    const bool isRoot = pic->isRoot;
    World& world = *e.world();

    if (!isRoot) {
        const Value* name = pe.find("name");
        if (name && name->getString() != e.name()) out.emplace_back("Name");
    }
    const Value* pcomps = componentsOf(pe);
    for (const ComponentInfo* info : ComponentRegistry::instance().componentsOf(world, e.handle())) {
        if (!info->serializable || info->name == kPrefabComponent) continue;
        if (isRoot && info->name == "Transform") continue;
        const Value* pv = pcomps ? pcomps->find(info->name) : nullptr;
        if (!pv) {
            out.push_back(info->name);
            continue;
        }
        const Value cur = info->serialize(world, e.handle());
        for (const auto& [field, v] : cur.fields()) {
            const Value* pf = pv->find(field);
            if (!pf || !(*pf == v)) out.push_back(info->name + "." + field);
        }
    }
    if (pcomps) {
        for (const auto& [name, v] : pcomps->fields()) {
            const ComponentInfo* info = ComponentRegistry::instance().find(name);
            if (info && !info->has(world, e.handle())) out.push_back("-" + name);
        }
    }
    return out;
}

usize recordDetectedOverrides(Entity instanceRoot, const serial::Document& prefab) {
    usize n = 0;
    for (Entity e : subtreeOf(instanceRoot)) {
        for (const auto& path : detectOverrides(e, prefab)) {
            if (!isOverridden(e, path)) {
                recordOverride(e, path);
                ++n;
            }
        }
    }
    return n;
}

bool revertOverride(Entity e, std::string_view propertyPath, const serial::Document& prefab) {
    auto* pic = e.tryGet<PrefabInstanceComponent>();
    const PrefabView view = viewOf(prefab);
    if (!pic || pic->prefab != view.id) return false;
    auto it = view.bySource.find(pic->sourceId);
    if (it == view.bySource.end()) return false;
    const Value pe = remappedEntity(*it->second, sourceToInstanceIds(instanceMap(prefabInstanceRoot(e), view.id)));
    World& world = *e.world();

    if (propertyPath == "Name") {
        if (const Value* name = pe.find("name")) e.setName(name->getString());
    } else {
        const bool removed = propertyPath.starts_with('-');
        const auto segments = splitPath(removed ? propertyPath.substr(1) : propertyPath);
        if (segments.empty()) return false;
        const std::string compName(segments.front());
        const Value* pcomps = componentsOf(pe);
        const Value* pv = pcomps ? pcomps->find(compName) : nullptr;
        const ComponentInfo* info = ComponentRegistry::instance().find(compName);
        if (!pv) {
            // Component added on the instance: reverting removes it.
            if (info && info->removable && segments.size() == 1) info->remove(world, e.handle());
        } else {
            restoreComponent(e, compName, *pv, std::span(segments).subspan(1));
        }
    }
    pic = e.tryGet<PrefabInstanceComponent>();
    const std::string path(propertyPath);
    std::erase_if(pic->overrides, [&](const std::string& o) { return o == path || o.starts_with(path + "."); });
    return true;
}

void revertAllOverrides(Entity instanceRoot, const serial::Document& prefab) {
    for (Entity e : subtreeOf(instanceRoot)) {
        auto* pic = e.tryGet<PrefabInstanceComponent>();
        if (!pic) continue;
        const auto paths = pic->overrides;
        for (const auto& p : paths) revertOverride(e, p, prefab);
    }
}

serial::Document applyInstanceToPrefab(World& world, Entity instanceRoot, const serial::Document& prefab) {
    const PrefabView view = viewOf(prefab);
    const auto subtree = subtreeOf(instanceRoot);
    std::unordered_map<Uuid, Uuid> toSource;
    for (Entity e : subtree) {
        auto* pic = e.tryGet<PrefabInstanceComponent>();
        if (pic && pic->prefab == view.id) {
            toSource.emplace(e.uuid(), pic->sourceId);
        } else {
            // Entity added on the instance becomes part of the prefab.
            const Uuid source = Uuid::generate();
            toSource.emplace(e.uuid(), source);
            e.addOrReplace<PrefabInstanceComponent>(PrefabInstanceComponent{view.id, source, false, {}});
        }
    }
    const Entity roots[] = {instanceRoot};
    Value entities = serializeEntities(world, roots, withoutPrefabLinks());
    remapEntityList(entities, toSource);

    // The instance root's placement and name are instance-specific: keep the prefab's own.
    if (!entities.items().empty()) {
        Value& rootValue = entities.items().front();
        if (auto old = view.bySource.find(view.rootSource); old != view.bySource.end()) {
            if (const Value* name = old->second->find("name")) rootValue.set("name", *name);
            const Value* oldComps = componentsOf(*old->second);
            Value* newComps = rootValue.find("components");
            if (oldComps && newComps) {
                if (const Value* t = oldComps->find("Transform")) newComps->set("Transform", *t);
            }
        }
    }

    serial::Document doc;
    doc.kind = "prefab";
    doc.version = kPrefabFormatVersion;
    doc.root = Value::makeObject("Prefab");
    doc.root.set("id", Value::makeUuid(view.id));
    doc.root.set("entities", std::move(entities));
    for (Entity e : subtree) {
        if (auto* pic = e.tryGet<PrefabInstanceComponent>()) pic->overrides.clear();
    }
    return doc;
}

usize updatePrefabInstances(World& world, const serial::Document& prefab) {
    const PrefabView view = viewOf(prefab);
    if (view.entities.empty()) return 0;
    std::vector<Entity> roots;
    for (auto [e, pic] : world.registry().view<PrefabInstanceComponent>().each()) {
        if (pic.isRoot && pic.prefab == view.id) roots.emplace_back(e, &world);
    }
    const auto& registry = ComponentRegistry::instance();
    for (Entity root : roots) {
        auto map = instanceMap(root, view.id);
        // 1. Entities added to the prefab (file order => parents first).
        for (const Value* pe : view.entities) {
            const Uuid source = pe->find("id")->getUuid();
            if (map.contains(source)) continue;
            const Value* parentRef = pe->find("parent");
            Entity parent = root;
            if (parentRef) {
                if (auto p = map.find(parentRef->getUuid()); p != map.end()) parent = p->second;
            }
            const Value* name = pe->find("name");
            Entity created = world.create(name ? name->getString() : std::string("Entity"), parent);
            created.add<PrefabInstanceComponent>(PrefabInstanceComponent{view.id, source, false, {}});
            map.emplace(source, created);
        }
        const auto ids = sourceToInstanceIds(map);

        // 2. Entities removed from the prefab.
        for (auto it = map.begin(); it != map.end();) {
            if (!view.bySource.contains(it->first) && it->second != root) {
                world.destroyImmediate(it->second);
                it = map.erase(it);
            } else {
                ++it;
            }
        }

        // 3. Sync hierarchy, names and components.
        for (const Value* rawPe : view.entities) {
            const Value pe = remappedEntity(*rawPe, ids);
            const Uuid source = rawPe->find("id")->getUuid();
            Entity inst = map.at(source);
            const bool isRoot = source == view.rootSource;
            const auto overrides = inst.get<PrefabInstanceComponent>().overrides;
            auto overridden = [&](std::string_view path) {
                return std::find(overrides.begin(), overrides.end(), path) != overrides.end();
            };

            if (!isRoot) {
                if (const Value* parentRef = pe.find("parent")) {
                    Entity desired = world.find(parentRef->getUuid());
                    if (desired.valid() && inst.parent() != desired) inst.setParent(desired, false);
                }
                if (const Value* name = pe.find("name"); name && !overridden("Name")) inst.setName(name->getString());
            }

            const Value* pcomps = componentsOf(pe);
            if (pcomps) {
                for (const auto& [compName, pv] : pcomps->fields()) {
                    if (isRoot && compName == "Transform") continue;
                    if (overridden(compName) || overridden("-" + compName)) continue;
                    const ComponentInfo* info = registry.find(compName);
                    if (!info) {
                        setUnknown(inst, compName, pv);
                        continue;
                    }
                    Value target = pv;
                    if (info->has(world, inst.handle())) {
                        const Value cur = info->serialize(world, inst.handle());
                        const std::string prefix = compName + ".";
                        for (const auto& o : overrides) {
                            if (!o.starts_with(prefix)) continue;
                            const auto segs = splitPath(std::string_view(o).substr(prefix.size()));
                            if (valueAt(cur, segs)) copyAt(target, cur, segs);
                        }
                    }
                    info->deserialize(world, inst.handle(), target);
                }
            }
            // Components removed from the prefab (and not added on purpose on the instance).
            for (const ComponentInfo* info : registry.componentsOf(world, inst.handle())) {
                if (!info->serializable || !info->removable || info->name == kPrefabComponent) continue;
                if (pcomps && pcomps->find(info->name)) continue;
                if (overridden(info->name)) continue;
                info->remove(world, inst.handle());
            }
        }
    }
    world.updateTransforms();
    return roots.size();
}

} // namespace ox
