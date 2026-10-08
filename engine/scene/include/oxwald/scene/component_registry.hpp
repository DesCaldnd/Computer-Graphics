#pragma once

#include <oxwald/core/reflect.hpp>
#include <oxwald/core/serial/convert.hpp>
#include <oxwald/scene/world.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// Component metadata for the editor, serializer and world cloning. Any module registers its components (after
// reflecting them) from its registerXxxTypes() function:
//
//   OX_REFLECT_TYPE(RigidBodyComponent, "RigidBody").field(...);
//   ox::ComponentRegistry::instance().add<RigidBodyComponent>({.category = "Physics", .icon = "cube"});
//
// Empty structs work as tag components (`struct EnemyTag {}; OX_REFLECT_TYPE(EnemyTag, "EnemyTag");`): they are
// saved/loaded, copied, shown in the inspector and queryable with world.registry().view<EnemyTag>().
//
// The registry is process-wide like the reflection TypeRegistry (it is type metadata, not an engine service).
namespace ox {

struct ComponentOptions {
    std::string name;     // defaults to the reflected type name
    std::string category; // defaults to the type's attr::Category
    std::string icon;     // defaults to the type's attr::Meta{"icon", ...}
    bool removable = true;
    bool hiddenInInspector = false;
    bool serializable = true;
};

struct ComponentInfo {
    std::string name;
    const reflect::TypeInfo* type = nullptr;
    std::string category;
    std::string icon;
    bool removable = true;
    bool hiddenInInspector = false;
    bool serializable = true;
    entt::id_type typeId = 0;

    std::function<bool(const World&, entt::entity)> has;
    // Adds a default-constructed component if absent; returns it.
    std::function<void*(World&, entt::entity)> add;
    std::function<void(World&, entt::entity)> remove;
    // Null when absent.
    std::function<void*(World&, entt::entity)> get;
    // Copies the component from src (must have it) to dst (added or replaced).
    std::function<void(World& dst, entt::entity, const World& src, entt::entity)> copy;
    // Fires entt update signals after an in-place edit (e.g. from the inspector through a ValueRef).
    std::function<void(World&, entt::entity)> notifyChanged;

    [[nodiscard]] reflect::ValueRef ref(World& world, entt::entity e) const {
        void* p = get(world, e);
        return p ? reflect::ValueRef{p, type} : reflect::ValueRef{};
    }
    [[nodiscard]] serial::Value serialize(const World& world, entt::entity e,
                                          const serial::ConvertOptions& options = {}) const;
    // Adds the component if needed and applies the value (tolerant, missing fields keep defaults).
    bool deserialize(World& world, entt::entity e, const serial::Value& value,
                     const serial::ConvertOptions& options = {}) const;
};

class ComponentRegistry {
public:
    static ComponentRegistry& instance();

    // Idempotent: re-adding a component type updates its options.
    template <class C>
    ComponentInfo& add(ComponentOptions options = {});

    [[nodiscard]] const ComponentInfo* find(std::string_view name) const;
    [[nodiscard]] const ComponentInfo* findByType(entt::id_type typeId) const;
    template <class C>
    [[nodiscard]] const ComponentInfo* find() const {
        return findByType(entt::type_hash<C>::value());
    }
    // In registration order.
    [[nodiscard]] std::vector<const ComponentInfo*> all() const;
    [[nodiscard]] std::vector<const ComponentInfo*> componentsOf(const World& world, entt::entity e) const;

private:
    ComponentRegistry() = default;
    ComponentInfo& upsert(ComponentInfo info);

    mutable std::mutex m_mutex;
    std::vector<std::unique_ptr<ComponentInfo>> m_components;
};

template <class C>
ComponentInfo& ComponentRegistry::add(ComponentOptions options) {
    const reflect::TypeInfo& type = reflect::typeOf<C>();
    OX_ASSERT(type.registered, "reflect component type before registering it");
    ComponentInfo info;
    info.name = options.name.empty() ? type.name : options.name;
    info.type = &type;
    info.category = options.category.empty() ? type.attributes.category : options.category;
    info.icon = options.icon.empty() ? std::string(type.attributes.getMeta("icon")) : options.icon;
    info.removable = options.removable;
    info.hiddenInInspector = options.hiddenInInspector || type.attributes.hidden;
    info.serializable = options.serializable;
    info.typeId = entt::type_hash<C>::value();
    info.has = [](const World& w, entt::entity e) { return w.registry().all_of<C>(e); };
    info.remove = [](World& w, entt::entity e) { w.registry().remove<C>(e); };
    if constexpr (std::is_empty_v<C>) {
        // Tag (marker) components: EnTT keeps no instances for empty types, so the registry hands out a shared
        // dummy object (no fields to read or write) while the entity has the tag.
        info.add = [](World& w, entt::entity e) -> void* {
            if (!w.registry().all_of<C>(e)) w.registry().emplace<C>(e);
            return &detail::tagInstance<C>();
        };
        info.get = [](World& w, entt::entity e) -> void* {
            return w.registry().all_of<C>(e) ? &detail::tagInstance<C>() : nullptr;
        };
        info.copy = [](World& dst, entt::entity de, const World&, entt::entity) {
            if (!dst.registry().all_of<C>(de)) dst.registry().emplace<C>(de);
        };
        info.notifyChanged = [](World&, entt::entity) {};
    } else {
        info.add = [](World& w, entt::entity e) -> void* { return &w.registry().get_or_emplace<C>(e); };
        info.get = [](World& w, entt::entity e) -> void* { return w.registry().try_get<C>(e); };
        info.copy = [](World& dst, entt::entity de, const World& src, entt::entity se) {
            dst.registry().emplace_or_replace<C>(de, src.registry().get<C>(se));
        };
        info.notifyChanged = [](World& w, entt::entity e) {
            if (w.registry().all_of<C>(e)) w.registry().patch<C>(e);
        };
    }
    return upsert(std::move(info));
}

} // namespace ox
