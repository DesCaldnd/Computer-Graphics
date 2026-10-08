#pragma once

#include <oxwald/physics/types.hpp>

#include <limits>

namespace ox::physics {

enum class ConstraintType : u8 {
    Fixed,    // welds two bodies at their current relative pose
    Point,    // ball joint: pointA on A coincides with pointB on B
    Hinge,    // rotation around `axis` through pointA; limits/motor in radians
    Slider,   // translation along `axis`; limits/motor in meters
    Distance, // keeps |pointA - pointB| within [minDistance, maxDistance]
    Cone,     // ball joint whose twist axis stays within a cone of coneHalfAngle
};

enum class MotorMode : u8 { Off, Velocity, Position };

// All points/axes are in WORLD space at creation time (the bodies' current poses).
// bodyB may be invalid → the constraint attaches bodyA to the static world.
struct ConstraintDesc {
    ConstraintType type = ConstraintType::Fixed;
    BodyHandle bodyA;
    BodyHandle bodyB;

    glm::vec3 pointA{0.f};
    glm::vec3 pointB{0.f};                // Point & Distance only; others use pointA for both
    glm::vec3 axis{0.f, 1.f, 0.f};        // hinge axis / slider axis / cone twist axis
    glm::vec3 normal{0.f};                // perpendicular reference axis; zero = derived from `axis`

    bool limitsEnabled = false;
    f32 limitMin = -3.14159265f; // Hinge: radians (≥ -π) / Slider: meters
    f32 limitMax = 3.14159265f;

    MotorMode motorMode = MotorMode::Off;
    f32 motorTarget = 0.f;       // velocity (rad/s or m/s) or position (rad or m)
    f32 motorMaxForce = 1000.f;  // N (slider) or N·m (hinge)
    f32 motorFrequency = 2.f;    // position motor spring frequency (Hz)
    f32 motorDamping = 1.f;

    f32 minDistance = -1.f; // Distance: < 0 → current distance
    f32 maxDistance = -1.f;
    f32 springFrequency = 0.f; // Distance: 0 = rigid limits
    f32 springDamping = 0.f;

    f32 coneHalfAngle = 0.5f; // Cone: radians

    // Breakable: disabled + ConstraintBrokenEvent when the constraint force/torque exceeds these.
    f32 breakForce = std::numeric_limits<f32>::infinity();
    f32 breakTorque = std::numeric_limits<f32>::infinity();
};

} // namespace ox::physics
