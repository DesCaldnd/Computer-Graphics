#pragma once

#include <oxwald/physics/shape.hpp>
#include <oxwald/physics/types.hpp>

namespace ox::physics {

// Degrees of freedom to lock (bit flags, combine with |). Same bit layout as JPH::EAllowedDOFs.
namespace lock {
inline constexpr u8 None = 0;
inline constexpr u8 TranslationX = 1 << 0;
inline constexpr u8 TranslationY = 1 << 1;
inline constexpr u8 TranslationZ = 1 << 2;
inline constexpr u8 RotationX = 1 << 3;
inline constexpr u8 RotationY = 1 << 4;
inline constexpr u8 RotationZ = 1 << 5;
inline constexpr u8 AllRotation = RotationX | RotationY | RotationZ;
inline constexpr u8 AllTranslation = TranslationX | TranslationY | TranslationZ;
// 2D physics in the XY plane.
inline constexpr u8 Plane2D = TranslationZ | RotationX | RotationY;
} // namespace lock

struct BodyDesc {
    ShapeRef shape;
    glm::vec3 position{0.f}; // shape origin (not the center of mass)
    glm::quat rotation{1.f, 0.f, 0.f, 0.f};
    glm::vec3 linearVelocity{0.f};
    glm::vec3 angularVelocity{0.f};

    MotionType motionType = MotionType::Dynamic;
    ObjectLayer layer = layers::Auto;
    bool isSensor = false; // trigger: reports enter/stay/exit, no collision response

    // Mass properties. mass <= 0: computed from the shape density. inertiaDiagonal == 0: inertia is
    // computed from the shape and scaled to `mass`.
    f32 mass = 0.f;
    glm::vec3 inertiaDiagonal{0.f};

    f32 friction = 0.5f;
    f32 restitution = 0.f;
    f32 linearDamping = 0.05f;
    f32 angularDamping = 0.05f;
    f32 gravityFactor = 1.f;
    f32 maxLinearVelocity = 500.f;
    f32 maxAngularVelocity = 0.25f * 3.14159265f * 60.f;

    bool ccd = false; // continuous collision detection (Jolt LinearCast motion quality)
    bool allowSleeping = true;
    bool startActive = true;
    bool reportContacts = true;      // generate ContactEvents for this body
    bool allowMotionTypeChange = false; // needed for setMotionType() on bodies created Static
    u8 lockAxes = lock::None;

    u64 userData = 0; // typically the entity id
};

} // namespace ox::physics
