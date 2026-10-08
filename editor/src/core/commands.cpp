#include "core/commands.hpp"

#include <oxwald/core/log.hpp>
#include <oxwald/core/reflect.hpp>
#include <oxwald/scene/component_registry.hpp>
#include <oxwald/scene/prefab.hpp>
#include <oxwald/scene/scene_serializer.hpp>

#include <algorithm>

namespace ox::editor {

// ---- generic property access --------------------------------------------------------------------------------

namespace props {

namespace {
reflect::ValueRef resolve(World& world, const Uuid& id, const std::string& component, const std::string& path,
                          const ComponentInfo** outInfo, Entity* outEntity) {
    Entity e = world.find(id);
    if (!e) return {};
    const ComponentInfo* info = ComponentRegistry::instance().find(component);
    if (!info) return {};
    reflect::ValueRef ref = info->ref(world, e.handle());
    if (!ref) return {};
    if (outInfo) *outInfo = info;
    if (outEntity) *outEntity = e;
    return path.empty() ? ref : reflect::resolvePath(ref, path);
}
} // namespace

std::optional<serial::Value> read(World& world, const Uuid& entity, const std::string& component, const std::string& path) {
    reflect::ValueRef ref = resolve(world, entity, component, path, nullptr, nullptr);
    if (!ref) return std::nullopt;
    return ref.get();
}

bool write(World& world, const Uuid& entity, const std::string& component, const std::string& path,
           const serial::Value& value) {
    const ComponentInfo* info = nullptr;
    Entity e;
    reflect::ValueRef ref = resolve(world, entity, component, path, &info, &e);
    if (!ref) return false;
    const bool ok = ref.set(value);
    info->notifyChanged(world, e.handle());
    if (component == "Transform") world.markTransformDirty(e.handle());
    return ok;
}

std::string overridePath(const std::string& component, const std::string& path) {
    if (component == "Name") return "Name";
    if (path.empty()) return component;
    size_t end = path.find_first_of(".[");
    return component + "." + path.substr(0, end);
}

} // namespace props

// ---- SetPropertyCommand -------------------------------------------------------------------------------------

SetPropertyCommand::SetPropertyCommand(ICommandHost& host, std::string component, std::string path,
                                       std::vector<Target> targets, bool open, const QString& text, QUndoCommand* parent)
    : QUndoCommand(text, parent), m_host(host), m_component(std::move(component)), m_path(std::move(path)),
      m_targets(std::move(targets)), m_open(open) {
    World& w = m_host.commandWorld();
    for (auto& t : m_targets) {
        if (auto* pi = w.find(t.entity).tryGet<PrefabInstanceComponent>()) t.overridesBefore = pi->overrides;
    }
}

void SetPropertyCommand::apply(bool forward) {
    World& w = m_host.commandWorld();
    for (const auto& t : m_targets) {
        props::write(w, t.entity, m_component, m_path, forward ? t.after : t.before);
        Entity e = w.find(t.entity);
        auto* pi = e.tryGet<PrefabInstanceComponent>();
        if (!pi) continue;
        if (!forward) {
            if (t.overridesBefore) pi->overrides = *t.overridesBefore;
            continue;
        }
        // Instance roots keep their transform/name per instance; everything else is a tracked override.
        const bool instanceSpecific = pi->isRoot && (m_component == "Transform" || m_component == "Name");
        if (!instanceSpecific && m_component != "PrefabInstance") recordOverride(e, props::overridePath(m_component, m_path));
    }
    m_host.onWorldEdited(m_component == "Name" ? EditKind::Structure : EditKind::Properties);
}

void SetPropertyCommand::redo() { apply(true); }
void SetPropertyCommand::undo() { apply(false); }

bool SetPropertyCommand::mergeWith(const QUndoCommand* other) {
    if (!m_open || other->id() != id()) return false;
    const auto* o = static_cast<const SetPropertyCommand*>(other);
    if (o->m_component != m_component || o->m_path != m_path || o->m_targets.size() != m_targets.size()) return false;
    for (size_t i = 0; i < m_targets.size(); ++i) {
        if (o->m_targets[i].entity != m_targets[i].entity) return false;
    }
    for (size_t i = 0; i < m_targets.size(); ++i) m_targets[i].after = o->m_targets[i].after;
    m_open = o->m_open;
    return true;
}

// ---- subtree snapshots --------------------------------------------------------------------------------------

namespace {
i32 siblingIndex(World& world, Entity e) {
    Entity parent = e.parent();
    std::span<const entt::entity> siblings = parent ? parent.childHandles() : world.rootHandles();
    auto it = std::find(siblings.begin(), siblings.end(), e.handle());
    return it == siblings.end() ? -1 : i32(it - siblings.begin());
}
} // namespace

SubtreeSnapshot captureSubtree(World& world, Entity root) {
    SubtreeSnapshot s;
    s.root = root.uuid();
    if (Entity p = root.parent()) s.parent = p.uuid();
    s.index = siblingIndex(world, root);
    Entity roots[] = {root};
    s.entities = serializeEntities(world, roots);
    return s;
}

Entity restoreSubtree(World& world, const SubtreeSnapshot& snapshot) {
    Entity parent = snapshot.parent.isNil() ? Entity{} : world.find(snapshot.parent);
    auto r = instantiateEntities(world, snapshot.entities, false, parent);
    if (!r) {
        OX_LOG_ERROR("editor", "cannot restore entities: {}", r.error().message);
        return {};
    }
    Entity root = world.find(snapshot.root);
    if (root) world.setParent(root.handle(), parent ? parent.handle() : entt::null, false, snapshot.index);
    return root;
}

UuidList topLevelOnly(World& world, const UuidList& ids) {
    UuidList out;
    for (const auto& id : ids) {
        Entity e = world.find(id);
        if (!e) continue;
        bool covered = false;
        for (const auto& other : ids) {
            if (other == id) continue;
            Entity o = world.find(other);
            if (o && o.isAncestorOf(e)) {
                covered = true;
                break;
            }
        }
        if (!covered && std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
    }
    return out;
}

// ---- create / delete ----------------------------------------------------------------------------------------

CreateEntitiesCommand::CreateEntitiesCommand(ICommandHost& host, Factory factory, const QString& text, QUndoCommand* parent)
    : QUndoCommand(text, parent), m_host(host), m_factory(std::move(factory)) {}

void CreateEntitiesCommand::redo() {
    World& w = m_host.commandWorld();
    if (!m_done) {
        m_done = true;
        std::vector<Entity> roots = m_factory(w);
        m_created.clear();
        for (Entity e : roots) {
            if (e) m_created.push_back(e.uuid());
        }
        m_created = topLevelOnly(w, m_created);
        w.updateTransforms();
        for (const auto& id : m_created) m_snapshots.push_back(captureSubtree(w, w.find(id)));
        m_factory = nullptr;
    } else {
        auto sorted = m_snapshots;
        std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.index < b.index; });
        for (const auto& s : sorted) restoreSubtree(w, s);
    }
    m_host.onWorldEdited(EditKind::Structure);
}

void CreateEntitiesCommand::undo() {
    World& w = m_host.commandWorld();
    for (auto it = m_created.rbegin(); it != m_created.rend(); ++it) {
        if (Entity e = w.find(*it)) w.destroyImmediate(e);
    }
    m_host.onWorldEdited(EditKind::Structure);
}

DeleteEntitiesCommand::DeleteEntitiesCommand(ICommandHost& host, const UuidList& ids, const QString& text, QUndoCommand* parent)
    : QUndoCommand(text, parent), m_host(host) {
    World& w = m_host.commandWorld();
    for (const auto& id : topLevelOnly(w, ids)) m_snapshots.push_back(captureSubtree(w, w.find(id)));
    // restore order: by parent, then ascending sibling index, so each insert lands at its original position
    std::stable_sort(m_snapshots.begin(), m_snapshots.end(), [](const auto& a, const auto& b) { return a.index < b.index; });
}

void DeleteEntitiesCommand::redo() {
    World& w = m_host.commandWorld();
    for (const auto& s : m_snapshots) {
        if (Entity e = w.find(s.root)) w.destroyImmediate(e);
    }
    m_host.onWorldEdited(EditKind::Structure);
}

void DeleteEntitiesCommand::undo() {
    World& w = m_host.commandWorld();
    for (const auto& s : m_snapshots) restoreSubtree(w, s);
    m_host.onWorldEdited(EditKind::Structure);
}

// ---- replace subtrees ----------------------------------------------------------------------------------------

ReplaceSubtreesCommand::ReplaceSubtreesCommand(ICommandHost& host, std::vector<SubtreeSnapshot> before,
                                               std::vector<SubtreeSnapshot> after, const QString& text, QUndoCommand* parent)
    : QUndoCommand(text, parent), m_host(host), m_before(std::move(before)), m_after(std::move(after)) {}

void ReplaceSubtreesCommand::swapTo(const std::vector<SubtreeSnapshot>& from, const std::vector<SubtreeSnapshot>& to) {
    World& w = m_host.commandWorld();
    for (const auto& s : from) {
        if (Entity e = w.find(s.root)) w.destroyImmediate(e);
    }
    auto sorted = to;
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.index < b.index; });
    for (const auto& s : sorted) restoreSubtree(w, s);
    m_host.onWorldEdited(EditKind::Structure);
}

void ReplaceSubtreesCommand::redo() {
    if (m_first) {
        m_first = false;
        m_host.onWorldEdited(EditKind::Structure);
        return;
    }
    swapTo(m_before, m_after);
}

void ReplaceSubtreesCommand::undo() { swapTo(m_after, m_before); }

// ---- reparent -----------------------------------------------------------------------------------------------

ReparentCommand::ReparentCommand(ICommandHost& host, const UuidList& ids, const Uuid& newParent, i32 index,
                                 const QString& text, QUndoCommand* parent)
    : QUndoCommand(text, parent), m_host(host), m_newParent(newParent), m_index(index) {
    World& w = m_host.commandWorld();
    Entity np = newParent.isNil() ? Entity{} : w.find(newParent);
    if (!newParent.isNil() && !np) return;
    for (const auto& id : topLevelOnly(w, ids)) {
        Entity e = w.find(id);
        if (!e || id == newParent) continue;
        if (np && e.isAncestorOf(np)) continue; // would create a cycle
        Item it;
        it.id = id;
        if (Entity p = e.parent()) it.oldParent = p.uuid();
        it.oldIndex = siblingIndex(w, e);
        it.oldLocal = e.localTransform().toTransform();
        m_items.push_back(it);
    }
}

void ReparentCommand::redo() {
    World& w = m_host.commandWorld();
    Entity np = m_newParent.isNil() ? Entity{} : w.find(m_newParent);
    i32 index = m_index;
    for (const auto& it : m_items) {
        Entity e = w.find(it.id);
        if (!e) continue;
        // moving down within the same parent: removing the entity first shifts the target index
        if (index >= 0 && it.oldParent == m_newParent && it.oldIndex >= 0 && it.oldIndex < index) --index;
        e.setParent(np, true, index);
        if (index >= 0) ++index;
    }
    m_host.onWorldEdited(EditKind::Structure);
}

void ReparentCommand::undo() {
    World& w = m_host.commandWorld();
    auto items = m_items;
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.oldIndex < b.oldIndex; });
    for (const auto& it : items) {
        Entity e = w.find(it.id);
        if (!e) continue;
        Entity op = it.oldParent.isNil() ? Entity{} : w.find(it.oldParent);
        e.setParent(op, false, it.oldIndex);
        e.setLocalTransform(it.oldLocal);
    }
    m_host.onWorldEdited(EditKind::Structure);
}

// ---- components ---------------------------------------------------------------------------------------------

ComponentCommand::ComponentCommand(ICommandHost& host, Op op, const UuidList& ids, std::string component,
                                   std::optional<serial::Value> initial, const QString& text, QUndoCommand* parent)
    : QUndoCommand(text, parent), m_host(host), m_op(op), m_component(std::move(component)), m_initial(std::move(initial)) {
    World& w = m_host.commandWorld();
    const ComponentInfo* info = ComponentRegistry::instance().find(m_component);
    if (!info) return;
    for (const auto& id : ids) {
        Entity e = w.find(id);
        if (!e) continue;
        const bool has = info->has(w, e.handle());
        if (op == Op::Add && !has) m_items.push_back({id, {}});
        if (op == Op::Remove && has && info->removable) m_items.push_back({id, info->serialize(w, e.handle())});
    }
}

void ComponentCommand::redo() {
    World& w = m_host.commandWorld();
    const ComponentInfo* info = ComponentRegistry::instance().find(m_component);
    if (!info) return;
    for (const auto& it : m_items) {
        Entity e = w.find(it.id);
        if (!e) continue;
        if (m_op == Op::Add) {
            info->add(w, e.handle());
            if (m_initial) info->deserialize(w, e.handle(), *m_initial);
            if (auto* pi = e.tryGet<PrefabInstanceComponent>(); pi && !pi->isRoot) recordOverride(e, m_component);
        } else {
            info->remove(w, e.handle());
        }
    }
    m_host.onWorldEdited(EditKind::Structure);
}

void ComponentCommand::undo() {
    World& w = m_host.commandWorld();
    const ComponentInfo* info = ComponentRegistry::instance().find(m_component);
    if (!info) return;
    for (const auto& it : m_items) {
        Entity e = w.find(it.id);
        if (!e) continue;
        if (m_op == Op::Add) info->remove(w, e.handle());
        else info->deserialize(w, e.handle(), it.snapshot);
    }
    m_host.onWorldEdited(EditKind::Structure);
}

} // namespace ox::editor
