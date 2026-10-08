#pragma once

#include <oxwald/core/types.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <compare>
#include <functional>

namespace ox::physics {

// Body handles wrap Jolt's BodyID (index + sequence number), so stale handles are detected.
struct BodyHandle {
    static constexpr u32 kInvalid = 0xffffffffu;
    u32 id = kInvalid;

    [[nodiscard]] constexpr bool valid() const { return id != kInvalid; }
    constexpr explicit operator bool() const { return valid(); }
    constexpr auto operator<=>(const BodyHandle&) const = default;
};

// Generational slot handles for objects owned by the world (constraints, characters).
struct ConstraintHandle {
    u32 index = 0xffffffffu;
    u32 generation = 0;
    [[nodiscard]] constexpr bool valid() const { return index != 0xffffffffu; }
    constexpr explicit operator bool() const { return valid(); }
    constexpr auto operator<=>(const ConstraintHandle&) const = default;
};

struct CharacterHandle {
    u32 index = 0xffffffffu;
    u32 generation = 0;
    [[nodiscard]] constexpr bool valid() const { return index != 0xffffffffu; }
    constexpr explicit operator bool() const { return valid(); }
    constexpr auto operator<=>(const CharacterHandle&) const = default;
};

using ObjectLayer = u16;
using LayerMask = u32;

namespace layers {
inline constexpr ObjectLayer Static = 0;
inline constexpr ObjectLayer Dynamic = 1;
inline constexpr ObjectLayer Kinematic = 2;
inline constexpr ObjectLayer Character = 3;
inline constexpr ObjectLayer Trigger = 4;
inline constexpr ObjectLayer Debris = 5;
inline constexpr ObjectLayer FirstUser = 6;
inline constexpr u32 kMaxLayers = 32;
// BodyDesc::layer default: pick Static/Kinematic/Dynamic/Trigger from the motion type / sensor flag.
inline constexpr ObjectLayer Auto = 0xffff;
} // namespace layers

inline constexpr LayerMask kAllLayers = ~LayerMask{0};
constexpr LayerMask layerBit(ObjectLayer layer) { return LayerMask{1} << layer; }

enum class MotionType : u8 { Static, Kinematic, Dynamic };

enum class Activation : u8 { Activate, DontActivate };

struct Transform {
    glm::vec3 position{0.f};
    glm::quat rotation{1.f, 0.f, 0.f, 0.f};
};

struct Aabb {
    glm::vec3 min{0.f};
    glm::vec3 max{0.f};
};

using Color = glm::vec4; // linear RGBA

} // namespace ox::physics

template <>
struct std::hash<ox::physics::BodyHandle> {
    std::size_t operator()(const ox::physics::BodyHandle& h) const noexcept { return std::hash<ox::u32>{}(h.id); }
};
