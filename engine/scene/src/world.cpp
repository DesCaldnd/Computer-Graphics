#include <oxwald/core/profile.hpp>
#include <oxwald/scene/component_registry.hpp>
#include <oxwald/scene/world.hpp>

#include <algorithm>

namespace ox {

// ---- Entity ------------------------------------------------------------------------------------------------

bool Entity::valid() const { return m_world != nullptr && m_handle != entt::null && m_world->m_registry.valid(m_handle); }

Uuid Entity::uuid() const {
    const auto* id = tryGet<IdComponent>();
    return id ? id->id : Uuid{};
}

const std::string& Entity::name() const {
    static const std::string kEmpty;
    const auto* n = tryGet<NameComponent>();
    return n ? n->name : kEmpty;
}

void Entity::setName(std::string name) const { m_world->m_registry.get_or_emplace<NameComponent>(m_handle).name = std::move(name); }

bool Entity::active() const {
    const auto* a = tryGet<ActiveComponent>();
    return !a || a->active;
}

void Entity::setActive(bool active) const { m_world->m_registry.get_or_emplace<ActiveComponent>(m_handle).active = active; }

bool Entity::activeInHierarchy() const {
    for (Entity e = *this; e.valid(); e = e.parent()) {
        if (!e.active()) return false;
    }
    return true;
}

Entity Entity::parent() const {
    const auto* h = tryGet<HierarchyComponent>();
    return h && h->parent != entt::null ? Entity{h->parent, m_world} : Entity{};
}

std::span<const entt::entity> Entity::childHandles() const {
    const auto* h = tryGet<HierarchyComponent>();
    return h ? std::span<const entt::entity>(h->children) : std::span<const entt::entity>{};
}

std::vector<Entity> Entity::children() const {
    std::vector<Entity> out;
    for (auto c : childHandles()) out.emplace_back(c, m_world);
    return out;
}

bool Entity::isAncestorOf(Entity other) const {
    for (Entity p = other.parent(); p.valid(); p = p.parent()) {
        if (p == *this) return true;
    }
    return false;
}

void Entity::setParent(Entity parent, bool keepWorldTransform, i32 index) const {
    OX_ASSERT(valid(), "setParent on invalid entity");
    OX_ASSERT(!parent.valid() || parent.world() == m_world, "parent belongs to another world");
    m_world->setParent(m_handle, parent.valid() ? parent.handle() : entt::null, keepWorldTransform, index);
}

TransformComponent& Entity::transform() const {
    m_world->markTransformDirty(m_handle);
    return get<TransformComponent>();
}

const TransformComponent& Entity::localTransform() const { return get<TransformComponent>(); }

void Entity::setLocalTransform(const Transform& t) const { transform().setTransform(t); }
void Entity::setPosition(glm::vec3 p) const { transform().position = p; }
void Entity::setRotation(glm::quat q) const { transform().rotation = q; }
void Entity::setScale(glm::vec3 s) const { transform().scale = s; }

glm::mat4 Entity::worldMatrix() const { return m_world->computeWorldMatrix(m_handle); }
Transform Entity::worldTransform() const { return Transform::fromMatrix(worldMatrix()); }
glm::vec3 Entity::worldPosition() const { return glm::vec3(worldMatrix()[3]); }
glm::quat Entity::worldRotation() const { return worldTransform().rotation; }

void Entity::setWorldTransform(const Transform& t) const {
    const Entity p = parent();
    if (!p.valid()) {
        setLocalTransform(t);
        return;
    }
    setLocalTransform(Transform::fromMatrix(glm::inverse(p.worldMatrix()) * t.toMatrix()));
}

void Entity::setWorldPosition(glm::vec3 pos) const {
    const Entity p = parent();
    transform().position = p.valid() ? glm::vec3(glm::inverse(p.worldMatrix()) * glm::vec4(pos, 1.0f)) : pos;
}

void Entity::setWorldRotation(glm::quat q) const {
    const Entity p = parent();
    transform().rotation = p.valid() ? glm::normalize(glm::inverse(p.worldRotation()) * q) : q;
}

void Entity::destroy() const {
    if (valid()) m_world->destroy(*this);
}

// ---- World -------------------------------------------------------------------------------------------------

World::World() {
    m_registry.on_construct<TransformComponent>().connect<&World::onTransformChanged>(*this);
    m_registry.on_update<TransformComponent>().connect<&World::onTransformChanged>(*this);
    m_registry.on_destroy<IdComponent>().connect<&World::onIdDestroyed>(*this);
}

World::~World() {
    m_registry.on_construct<TransformComponent>().disconnect(this);
    m_registry.on_update<TransformComponent>().disconnect(this);
    m_registry.on_destroy<IdComponent>().disconnect(this);
    m_registry.clear();
}

void World::onTransformChanged(entt::registry& r, entt::entity e) {
    if (!r.all_of<TransformDirtyTag>(e)) r.emplace<TransformDirtyTag>(e);
}

void World::onIdDestroyed(entt::registry& r, entt::entity e) {
    const auto& id = r.get<IdComponent>(e);
    auto it = m_byUuid.find(id.id);
    if (it != m_byUuid.end() && it->second == e) m_byUuid.erase(it);
}

Entity World::create(std::string_view name, Entity parent) { return createWithId(Uuid::generate(), name, parent); }

Entity World::createWithId(Uuid id, std::string_view name, Entity parent) {
    OX_ASSERT(id.isValid(), "entity UUID must not be nil");
    OX_ASSERT(!m_byUuid.contains(id), "duplicate entity UUID {}", id.toString());
    const entt::entity e = m_registry.create();
    m_registry.emplace<IdComponent>(e, id);
    m_registry.emplace<NameComponent>(e, std::string(name));
    m_registry.emplace<HierarchyComponent>(e);
    m_registry.emplace<WorldTransformComponent>(e);
    m_registry.emplace<ActiveComponent>(e);
    m_registry.emplace<TransformComponent>(e);
    m_byUuid.emplace(id, e);
    m_roots.push_back(e);
    if (parent.valid()) setParent(e, parent.handle(), false);
    return {e, this};
}

void World::setUuid(entt::entity e, Uuid id) {
    auto& comp = m_registry.get_or_emplace<IdComponent>(e);
    if (comp.id == id) {
        m_byUuid[id] = e;
        return;
    }
    OX_ASSERT(!m_byUuid.contains(id), "duplicate entity UUID {}", id.toString());
    if (auto it = m_byUuid.find(comp.id); it != m_byUuid.end() && it->second == e) m_byUuid.erase(it);
    comp.id = id;
    m_byUuid.emplace(id, e);
}

void World::collectSubtree(entt::entity e, std::vector<entt::entity>& out) const {
    out.push_back(e);
    if (const auto* h = m_registry.try_get<HierarchyComponent>(e)) {
        for (auto c : h->children) collectSubtree(c, out);
    }
}

void World::destroy(Entity entity) {
    if (!entity.valid()) return;
    std::vector<entt::entity> subtree;
    collectSubtree(entity.handle(), subtree);
    for (auto e : subtree) {
        if (!m_registry.all_of<PendingDestroyTag>(e)) m_registry.emplace<PendingDestroyTag>(e);
    }
}

void World::detachFromParent(entt::entity e) {
    auto* h = m_registry.try_get<HierarchyComponent>(e);
    if (h && h->parent != entt::null) {
        if (auto* ph = m_registry.try_get<HierarchyComponent>(h->parent)) std::erase(ph->children, e);
        h->parent = entt::null;
    } else {
        std::erase(m_roots, e);
    }
}

void World::destroyImmediate(Entity entity) {
    if (!entity.valid()) return;
    std::vector<entt::entity> subtree;
    collectSubtree(entity.handle(), subtree);
    detachFromParent(entity.handle());
    for (auto it = subtree.rbegin(); it != subtree.rend(); ++it) m_registry.destroy(*it);
}

usize World::flushDestroyed() {
    auto view = m_registry.view<PendingDestroyTag>();
    std::vector<entt::entity> pending(view.begin(), view.end());
    usize count = 0;
    for (auto e : pending) {
        if (!m_registry.valid(e)) continue; // already removed as part of an ancestor's subtree
        // Only destroy subtree tops; descendants go with them.
        const auto* h = m_registry.try_get<HierarchyComponent>(e);
        if (h && h->parent != entt::null && m_registry.all_of<PendingDestroyTag>(h->parent)) continue;
        std::vector<entt::entity> subtree;
        collectSubtree(e, subtree);
        detachFromParent(e);
        for (auto it = subtree.rbegin(); it != subtree.rend(); ++it) m_registry.destroy(*it);
        count += subtree.size();
    }
    return count;
}

void World::clear() {
    m_registry.clear();
    m_byUuid.clear();
    m_roots.clear();
}

Entity World::find(Uuid id) const {
    auto it = m_byUuid.find(id);
    return it == m_byUuid.end() ? Entity{} : Entity{it->second, const_cast<World*>(this)};
}

Entity World::findByName(std::string_view name) const {
    Entity found;
    forEachInHierarchy([&](entt::entity e) {
        if (found.valid()) return;
        const auto* n = m_registry.try_get<NameComponent>(e);
        if (n && n->name == name) found = Entity{e, const_cast<World*>(this)};
    });
    return found;
}

usize World::entityCount() const { return m_registry.view<IdComponent>().size(); }

std::vector<Entity> World::roots() const {
    std::vector<Entity> out;
    for (auto e : m_roots) out.emplace_back(e, const_cast<World*>(this));
    return out;
}

void World::setParent(entt::entity child, entt::entity parent, bool keepWorldTransform, i32 index) {
    OX_ASSERT(m_registry.valid(child), "setParent: invalid child");
    OX_ASSERT(child != parent, "entity cannot be its own parent");
    if (parent != entt::null) {
        OX_ASSERT(m_registry.valid(parent), "setParent: invalid parent");
        for (entt::entity p = parent; p != entt::null; p = m_registry.get<HierarchyComponent>(p).parent) {
            OX_ASSERT(p != child, "setParent would create a cycle");
        }
    }
    const glm::mat4 world = keepWorldTransform ? computeWorldMatrix(child) : glm::mat4(1.0f);
    detachFromParent(child);
    auto& h = m_registry.get<HierarchyComponent>(child);
    h.parent = parent;
    std::vector<entt::entity>& siblings =
        parent != entt::null ? m_registry.get<HierarchyComponent>(parent).children : m_roots;
    if (index < 0 || usize(index) >= siblings.size()) {
        siblings.push_back(child);
    } else {
        siblings.insert(siblings.begin() + index, child);
    }
    if (keepWorldTransform) {
        const glm::mat4 parentWorld = parent != entt::null ? computeWorldMatrix(parent) : glm::mat4(1.0f);
        auto& t = m_registry.get<TransformComponent>(child);
        t.setTransform(Transform::fromMatrix(glm::inverse(parentWorld) * world));
    }
    markTransformDirty(child);
}

glm::mat4 World::computeWorldMatrix(entt::entity e) const {
    glm::mat4 m(1.0f);
    // Walk up and accumulate parent * ... * local.
    for (entt::entity cur = e; cur != entt::null;) {
        if (const auto* t = m_registry.try_get<TransformComponent>(cur)) m = t->matrix() * m;
        const auto* h = m_registry.try_get<HierarchyComponent>(cur);
        cur = h ? h->parent : entt::null;
    }
    return m;
}

void World::markTransformDirty(entt::entity e) {
    if (!m_registry.all_of<TransformDirtyTag>(e)) m_registry.emplace<TransformDirtyTag>(e);
}

usize World::updateTransforms() {
    OX_PROFILE_ZONE();
    auto dirty = m_registry.view<TransformDirtyTag>();
    if (dirty.begin() == dirty.end()) return 0;
    // Recompute from each top-most dirty entity down its subtree (descendants depend on it).
    std::vector<entt::entity> tops;
    for (auto e : dirty) {
        bool ancestorDirty = false;
        const auto* h = m_registry.try_get<HierarchyComponent>(e);
        for (entt::entity p = h ? h->parent : entt::null; p != entt::null;) {
            if (m_registry.all_of<TransformDirtyTag>(p)) {
                ancestorDirty = true;
                break;
            }
            const auto* ph = m_registry.try_get<HierarchyComponent>(p);
            p = ph ? ph->parent : entt::null;
        }
        if (!ancestorDirty) tops.push_back(e);
    }
    usize updated = 0;
    std::vector<std::pair<entt::entity, glm::mat4>> stack;
    for (auto top : tops) {
        const auto* h = m_registry.try_get<HierarchyComponent>(top);
        const glm::mat4 parentWorld = h && h->parent != entt::null
                                          ? (m_registry.all_of<WorldTransformComponent>(h->parent)
                                                 ? m_registry.get<WorldTransformComponent>(h->parent).matrix
                                                 : computeWorldMatrix(h->parent))
                                          : glm::mat4(1.0f);
        stack.emplace_back(top, parentWorld);
        while (!stack.empty()) {
            auto [e, parentMatrix] = stack.back();
            stack.pop_back();
            const auto* t = m_registry.try_get<TransformComponent>(e);
            const glm::mat4 world = t ? parentMatrix * t->matrix() : parentMatrix;
            if (auto* wt = m_registry.try_get<WorldTransformComponent>(e)) wt->matrix = world;
            ++updated;
            if (const auto* eh = m_registry.try_get<HierarchyComponent>(e)) {
                for (auto c : eh->children) stack.emplace_back(c, world);
            }
        }
    }
    m_registry.clear<TransformDirtyTag>();
    return updated;
}

void World::snapshotPreviousTransforms() {
    for (auto [e, wt] : m_registry.view<WorldTransformComponent>().each()) wt.previous = wt.matrix;
}

std::unique_ptr<World> World::clone() const {
    OX_PROFILE_ZONE();
    auto out = std::make_unique<World>();
    // Recreate identical entity identifiers so stored entt handles (hierarchy links) stay valid.
    std::vector<entt::entity> entities;
    if (const auto* storage = m_registry.storage<entt::entity>()) {
        for (auto [e] : storage->each()) entities.push_back(e);
    }
    std::sort(entities.begin(), entities.end(), [](entt::entity a, entt::entity b) {
        return entt::to_entity(a) < entt::to_entity(b);
    });
    for (auto e : entities) {
        const entt::entity created = out->m_registry.create(e);
        OX_ASSERT(created == e, "world clone could not reproduce entity id");
    }
    const auto components = ComponentRegistry::instance().all();
    for (auto e : entities) {
        for (const ComponentInfo* info : components) {
            if (info->has(*this, e)) info->copy(*out, e, *this, e);
        }
        if (m_registry.all_of<TransformDirtyTag>(e) && !out->m_registry.all_of<TransformDirtyTag>(e)) {
            out->m_registry.emplace<TransformDirtyTag>(e);
        } else if (!m_registry.all_of<TransformDirtyTag>(e)) {
            out->m_registry.remove<TransformDirtyTag>(e);
        }
        if (m_registry.all_of<PendingDestroyTag>(e)) out->m_registry.emplace<PendingDestroyTag>(e);
    }
    // Drop entities that exist only as recycled slots in the source (none expected) and rebuild indices.
    out->m_byUuid = m_byUuid;
    out->m_roots = m_roots;
    return out;
}

} // namespace ox
