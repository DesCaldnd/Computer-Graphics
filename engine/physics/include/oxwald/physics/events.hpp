#pragma once

#include <oxwald/physics/types.hpp>

#include <array>

namespace ox::physics {

inline constexpr u32 kMaxContactEventPoints = 4;

enum class ContactEventType : u8 { Begin, Persist, End };

// Contact between two non-sensor bodies, aggregated per body pair (compound sub-shapes are merged).
// Collected thread-safely during step() and dispatched on the calling thread afterwards, sorted so
// the order is deterministic. bodyA.id < bodyB.id.
struct ContactEvent {
    ContactEventType type = ContactEventType::Begin;
    BodyHandle bodyA;
    BodyHandle bodyB;
    u64 userDataA = 0;
    u64 userDataB = 0;
    // Begin/Persist only:
    glm::vec3 normal{0.f}; // world space, pointing from A to B
    f32 penetration = 0.f;
    u32 pointCount = 0;
    std::array<glm::vec3, kMaxContactEventPoints> points{}; // world space, on the surface of B
    f32 normalImpulse = 0.f; // Begin: estimated total impulse (N·s) of the collision; Persist: 0
};

enum class TriggerEventType : u8 { Enter, Stay, Exit };

// Overlap of a sensor body with another body. Stay is emitted every step after Enter while overlapping.
struct TriggerEvent {
    TriggerEventType type = TriggerEventType::Enter;
    BodyHandle trigger;
    BodyHandle other;
    u64 triggerUserData = 0;
    u64 otherUserData = 0;
};

struct ConstraintBrokenEvent {
    ConstraintHandle constraint;
    f32 force = 0.f;  // N, at the moment it broke
    f32 torque = 0.f; // N·m
};

} // namespace ox::physics
