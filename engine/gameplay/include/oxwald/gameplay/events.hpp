#pragma once

#include <oxwald/core/types.hpp>
#include <oxwald/scene/world.hpp>

#include <glm/vec3.hpp>

#include <string>

// Gameplay events with entities. Each runtime emits them through an ox::Signal member (synchronously, on the
// simulation thread) and, when an ox::EventBus service exists, publishes them on it too. Lua scripts receive
// them as callbacks (onCollisionEnter, onTriggerEnter, onSplineEvent, onAnimationEvent, ...).
namespace ox::gameplay {

enum class ContactPhase : u8 { Begin, Persist, End };

struct CollisionEvent {
    ContactPhase phase = ContactPhase::Begin;
    Entity a; // a < b by body id (physics order)
    Entity b;
    glm::vec3 normal{0.f}; // from a to b (Begin/Persist)
    glm::vec3 point{0.f};  // first contact point (Begin/Persist)
    f32 impulse = 0.f;     // Begin: estimated impulse (N*s)
};

enum class TriggerPhase : u8 { Enter, Stay, Exit };

struct TriggerEvent {
    TriggerPhase phase = TriggerPhase::Enter;
    Entity trigger;
    Entity other;
};

struct JointBrokenEvent {
    Entity entity; // owner of the JointComponent
};

struct SplineFollowerEvent {
    Entity follower;
    std::string name;
    f32 distance = 0.f;
    i32 direction = 1;
    bool marker = false;
};

struct AnimationEvent {
    Entity entity;
    std::string name;
    f32 payload = 0.f;
    f32 weight = 1.f;
};

struct PerceptionEvent {
    Entity listener;
    Entity source; // may be invalid for anonymous noises
    bool gained = true;
    bool sight = true; // false: hearing
};

} // namespace ox::gameplay
