#pragma once

#include <oxwald/physics/shape.hpp>
#include <oxwald/physics/types.hpp>

namespace ox::physics {

// Kinematic, collide-and-slide character (JPH::CharacterVirtual). Position = feet (bottom of the capsule).
struct CharacterDesc {
    f32 height = 1.8f; // total capsule height
    f32 radius = 0.3f;
    ShapeRef customShape; // optional: replaces the capsule; origin should be at the feet
    glm::vec3 position{0.f};
    glm::quat rotation{1.f, 0.f, 0.f, 0.f};
    glm::vec3 up{0.f, 1.f, 0.f};

    f32 maxSlopeAngle = 0.7853982f; // radians (45°); steeper surfaces are walls
    f32 maxStepHeight = 0.35f;      // stairs auto step-up; 0 disables
    f32 stickToFloorDistance = 0.5f; // snap down when walking off small ledges/slopes; 0 disables
    f32 mass = 70.f;                // used to push bodies down when standing on them
    f32 maxStrength = 100.f;        // max push force on dynamic bodies (N)
    f32 characterPadding = 0.02f;
    f32 penetrationRecoverySpeed = 1.f;
    f32 predictiveContactDistance = 0.1f;

    ObjectLayer layer = layers::Character;
    // Kinematic proxy body so raycasts, triggers and dynamic bodies see the character.
    bool innerBody = true;
    u64 userData = 0;
};

enum class GroundState : u8 {
    OnGround,      // standing on walkable ground
    OnSteepGround, // touching ground steeper than maxSlopeAngle (slides down)
    NotSupported,  // touching something but not supported (e.g. against a wall in mid air)
    InAir,
};

struct CharacterMoveInput {
    glm::vec3 desiredVelocity{0.f}; // horizontal, world space (m/s)
    bool jump = false;
    f32 jumpSpeed = 5.f;
    f32 airControl = 0.25f; // 0 = keep air velocity, 1 = full control while airborne
};

struct CharacterState {
    glm::vec3 position{0.f};
    glm::quat rotation{1.f, 0.f, 0.f, 0.f};
    glm::vec3 linearVelocity{0.f};
    GroundState groundState = GroundState::InAir;
    glm::vec3 groundNormal{0.f};
    glm::vec3 groundVelocity{0.f};
    BodyHandle groundBody;
    u64 groundUserData = 0;
    bool slopeTooSteep = false;
};

} // namespace ox::physics
