#pragma once

#include <oxwald/core/uuid.hpp>

#include <functional>

namespace ox {

// Persistent reference to an entity by UUID. Use it in reflected component fields that point at other
// entities; it is stored as an archive EntityRef and remapped when scenes are pasted or prefabs instantiated.
// Resolve with World::resolve(ref).
struct EntityRef {
    Uuid id;

    EntityRef() = default;
    explicit EntityRef(Uuid uuid) : id(uuid) {}

    [[nodiscard]] bool valid() const { return id.isValid(); }
    explicit operator bool() const { return valid(); }
    friend bool operator==(const EntityRef&, const EntityRef&) = default;
};

} // namespace ox

template <>
struct std::hash<ox::EntityRef> {
    std::size_t operator()(const ox::EntityRef& r) const noexcept { return std::hash<ox::Uuid>{}(r.id); }
};
