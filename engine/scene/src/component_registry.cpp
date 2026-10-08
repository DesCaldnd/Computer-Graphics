#include <oxwald/scene/component_registry.hpp>

namespace ox {

ComponentRegistry& ComponentRegistry::instance() {
    static ComponentRegistry* registry = new ComponentRegistry();
    return *registry;
}

ComponentInfo& ComponentRegistry::upsert(ComponentInfo info) {
    std::lock_guard lock(m_mutex);
    for (auto& existing : m_components) {
        if (existing->typeId == info.typeId) {
            *existing = std::move(info);
            return *existing;
        }
    }
    for (auto& existing : m_components) {
        OX_ASSERT(existing->name != info.name, "component name '{}' registered for two types", info.name);
    }
    m_components.push_back(std::make_unique<ComponentInfo>(std::move(info)));
    return *m_components.back();
}

const ComponentInfo* ComponentRegistry::find(std::string_view name) const {
    std::lock_guard lock(m_mutex);
    for (const auto& c : m_components) {
        if (c->name == name) return c.get();
    }
    return nullptr;
}

const ComponentInfo* ComponentRegistry::findByType(entt::id_type typeId) const {
    std::lock_guard lock(m_mutex);
    for (const auto& c : m_components) {
        if (c->typeId == typeId) return c.get();
    }
    return nullptr;
}

std::vector<const ComponentInfo*> ComponentRegistry::all() const {
    std::lock_guard lock(m_mutex);
    std::vector<const ComponentInfo*> out;
    out.reserve(m_components.size());
    for (const auto& c : m_components) out.push_back(c.get());
    return out;
}

std::vector<const ComponentInfo*> ComponentRegistry::componentsOf(const World& world, entt::entity e) const {
    std::vector<const ComponentInfo*> out;
    for (const auto* c : all()) {
        if (c->has(world, e)) out.push_back(c);
    }
    return out;
}

serial::Value ComponentInfo::serialize(const World& world, entt::entity e, const serial::ConvertOptions& options) const {
    const void* p = get(const_cast<World&>(world), e);
    OX_ASSERT(p != nullptr, "serialize: entity has no {} component", name);
    return serial::toValue(p, *type, options);
}

bool ComponentInfo::deserialize(World& world, entt::entity e, const serial::Value& value,
                                const serial::ConvertOptions& options) const {
    void* p = add(world, e);
    const bool ok = serial::fromValue(value, p, *type, options);
    notifyChanged(world, e);
    return ok;
}

} // namespace ox
