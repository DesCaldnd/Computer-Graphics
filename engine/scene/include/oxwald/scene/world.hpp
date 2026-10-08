#pragma once

#include <oxwald/core/assert.hpp>
#include <oxwald/core/math.hpp>
#include <oxwald/core/uuid.hpp>
#include <oxwald/scene/components.hpp>
#include <oxwald/scene/entity_ref.hpp>

#include <entt/entity/registry.hpp>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ox {

class World;

// Lightweight handle (entt id + world pointer). Copyable, compares by value. A default Entity is invalid.
class Entity {
public:
    Entity() = default;
    Entity(entt::entity handle, World* world) : m_handle(handle), m_world(world) {}

    [[nodiscard]] bool valid() const;
    explicit operator bool() const { return valid(); }
    [[nodiscard]] entt::entity handle() const { return m_handle; }
    [[nodiscard]] World* world() const { return m_world; }
    friend bool operator==(const Entity& a, const Entity& b) { return a.m_handle == b.m_handle && a.m_world == b.m_world; }

    // ---- components ----
    template <class C, class... Args>
    C& add(Args&&... args);
    template <class C, class... Args>
    C& addOrReplace(Args&&... args);
    template <class C>
    [[nodiscard]] C& get() const;
    template <class C>
    [[nodiscard]] C* tryGet() const;
    template <class... C>
    [[nodiscard]] bool has() const;
    template <class... C>
    [[nodiscard]] bool hasAny() const;
    template <class C>
    void remove() const;
    // Applies fn to the component and fires entt update signals (marks transforms dirty for TransformComponent).
    template <class C, class Fn>
    void patch(Fn&& fn) const;

    // ---- identity ----
    [[nodiscard]] Uuid uuid() const;
    [[nodiscard]] EntityRef ref() const { return EntityRef{uuid()}; }
    [[nodiscard]] const std::string& name() const;
    void setName(std::string name) const;
    [[nodiscard]] bool active() const;
    void setActive(bool active) const;
    // False when the entity or any ancestor is inactive.
    [[nodiscard]] bool activeInHierarchy() const;

    // ---- hierarchy ----
    [[nodiscard]] Entity parent() const;
    [[nodiscard]] std::span<const entt::entity> childHandles() const;
    [[nodiscard]] std::vector<Entity> children() const;
    [[nodiscard]] usize childCount() const { return childHandles().size(); }
    [[nodiscard]] bool isAncestorOf(Entity other) const;
    // index < 0 appends. keepWorldTransform recomputes the local transform so the entity does not move.
    void setParent(Entity parent, bool keepWorldTransform = true, i32 index = -1) const;

    // ---- transforms ----
    // Mutable local transform; marks the world transform dirty (assumes modification).
    [[nodiscard]] TransformComponent& transform() const;
    [[nodiscard]] const TransformComponent& localTransform() const;
    void setLocalTransform(const Transform& t) const;
    void setPosition(glm::vec3 p) const;
    void setRotation(glm::quat q) const;
    void setScale(glm::vec3 s) const;
    // World space (computed through the hierarchy, independent of the cache).
    [[nodiscard]] glm::mat4 worldMatrix() const;
    [[nodiscard]] Transform worldTransform() const;
    [[nodiscard]] glm::vec3 worldPosition() const;
    [[nodiscard]] glm::quat worldRotation() const;
    void setWorldTransform(const Transform& t) const;
    void setWorldPosition(glm::vec3 p) const;
    void setWorldRotation(glm::quat q) const;

    // Deferred: destroyed with its subtree at World::flushDestroyed() (end of frame).
    void destroy() const;

private:
    entt::entity m_handle{entt::null};
    World* m_world = nullptr;
};

// ECS world wrapping an entt::registry. Every entity created through create() has Id, Name, Transform,
// Hierarchy, WorldTransform and Active components. Entities are indexed by UUID.
class World {
public:
    World();
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    Entity create(std::string_view name = "Entity", Entity parent = {});
    Entity createWithId(Uuid id, std::string_view name = "Entity", Entity parent = {});
    // Deferred destruction (entity + subtree) at flushDestroyed().
    void destroy(Entity entity);
    // Immediate destruction of the entity and its subtree.
    void destroyImmediate(Entity entity);
    usize flushDestroyed();
    void clear();

    [[nodiscard]] bool valid(entt::entity e) const { return m_registry.valid(e); }
    [[nodiscard]] Entity wrap(entt::entity e) { return {e, this}; }
    [[nodiscard]] Entity find(Uuid id) const;
    [[nodiscard]] Entity resolve(const EntityRef& ref) const { return find(ref.id); }
    [[nodiscard]] Entity findByName(std::string_view name) const;
    [[nodiscard]] usize entityCount() const;
    // Roots in their sibling order.
    [[nodiscard]] std::span<const entt::entity> rootHandles() const { return m_roots; }
    [[nodiscard]] std::vector<Entity> roots() const;
    // Pre-order (parents before children, sibling order preserved) over all entities.
    template <class Fn>
    void forEachInHierarchy(Fn&& fn) const;

    [[nodiscard]] entt::registry& registry() { return m_registry; }
    [[nodiscard]] const entt::registry& registry() const { return m_registry; }
    template <class... C>
    [[nodiscard]] auto view() {
        return m_registry.view<C...>();
    }

    // Hierarchy (see Entity::setParent).
    void setParent(entt::entity child, entt::entity parent, bool keepWorldTransform = true, i32 index = -1);
    [[nodiscard]] glm::mat4 computeWorldMatrix(entt::entity e) const;
    void markTransformDirty(entt::entity e);

    // Recomputes cached world matrices of dirty entities (and their subtrees).
    usize updateTransforms();
    // Copies every cached world matrix to `previous` (call once per frame before simulation).
    void snapshotPreviousTransforms();

    // Deep copy of all entities with registered components (same entity ids and UUIDs). Used for play-in-editor.
    [[nodiscard]] std::unique_ptr<World> clone() const;

    // Internal: UUID index maintenance (used by the serializer when re-identifying entities).
    void setUuid(entt::entity e, Uuid id);

private:
    friend class Entity;
    void detachFromParent(entt::entity e);
    void collectSubtree(entt::entity e, std::vector<entt::entity>& out) const;
    void onTransformChanged(entt::registry& r, entt::entity e);
    void onIdDestroyed(entt::registry& r, entt::entity e);

    entt::registry m_registry;
    std::unordered_map<Uuid, entt::entity> m_byUuid;
    std::vector<entt::entity> m_roots;
};

// ---- Entity template implementation -----------------------------------------------------------------------

template <class C, class... Args>
C& Entity::add(Args&&... args) {
    OX_ASSERT(valid(), "add on invalid entity");
    OX_ASSERT(!m_world->m_registry.all_of<C>(m_handle), "entity already has this component");
    return m_world->m_registry.emplace<C>(m_handle, std::forward<Args>(args)...);
}

template <class C, class... Args>
C& Entity::addOrReplace(Args&&... args) {
    OX_ASSERT(valid(), "addOrReplace on invalid entity");
    return m_world->m_registry.emplace_or_replace<C>(m_handle, std::forward<Args>(args)...);
}

template <class C>
C& Entity::get() const {
    OX_ASSERT(valid(), "get on invalid entity");
    return m_world->m_registry.get<C>(m_handle);
}

template <class C>
C* Entity::tryGet() const {
    return valid() ? m_world->m_registry.try_get<C>(m_handle) : nullptr;
}

template <class... C>
bool Entity::has() const {
    return valid() && m_world->m_registry.all_of<C...>(m_handle);
}

template <class... C>
bool Entity::hasAny() const {
    return valid() && m_world->m_registry.any_of<C...>(m_handle);
}

template <class C>
void Entity::remove() const {
    if (valid()) m_world->m_registry.remove<C>(m_handle);
}

template <class C, class Fn>
void Entity::patch(Fn&& fn) const {
    OX_ASSERT(valid(), "patch on invalid entity");
    m_world->m_registry.patch<C>(m_handle, std::forward<Fn>(fn));
}

template <class Fn>
void World::forEachInHierarchy(Fn&& fn) const {
    std::vector<entt::entity> stack(m_roots.rbegin(), m_roots.rend());
    while (!stack.empty()) {
        const entt::entity e = stack.back();
        stack.pop_back();
        fn(e);
        if (const auto* h = m_registry.try_get<HierarchyComponent>(e)) {
            stack.insert(stack.end(), h->children.rbegin(), h->children.rend());
        }
    }
}

} // namespace ox
