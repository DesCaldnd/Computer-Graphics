#pragma once

#include <oxwald/physics/types.hpp>

#include <string_view>

namespace ox::physics {

// Receiver for physics debug geometry. Adapt to ox::DebugDraw (core) or any renderer.
class PhysicsDebugSink {
public:
    virtual ~PhysicsDebugSink() = default;
    virtual void line(const glm::vec3& a, const glm::vec3& b, const Color& color) = 0;
    virtual void triangle(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const Color& color) {
        line(a, b, color);
        line(b, c, color);
        line(c, a, color);
    }
    virtual void text(const glm::vec3& position, std::string_view text, const Color& color) {}
};

struct DebugDrawOptions {
    bool shapes = true;
    bool aabbs = false;
    bool contacts = false;      // points + normals of the last step's Begin/Persist contact events
    bool velocities = false;
    bool centerOfMass = false;
    bool constraints = true;
    bool characters = true;
    bool labels = false;        // body id / user data as text
    bool colorSleeping = true;
    bool filledTriangles = false; // meshes/height fields via triangle() instead of wireframe
    u32 maxTrianglesPerBody = 20000;
    f32 velocityScale = 0.1f;
    f32 contactNormalLength = 0.3f;

    Color staticColor{0.6f, 0.6f, 0.6f, 1.f};
    Color dynamicColor{0.2f, 0.9f, 0.3f, 1.f};
    Color kinematicColor{0.3f, 0.5f, 1.f, 1.f};
    Color sleepingColor{0.35f, 0.4f, 0.35f, 1.f};
    Color sensorColor{1.f, 0.85f, 0.1f, 1.f};
    Color characterColor{0.9f, 0.4f, 1.f, 1.f};
    Color aabbColor{1.f, 0.5f, 0.f, 1.f};
    Color contactColor{1.f, 0.1f, 0.1f, 1.f};
    Color velocityColor{0.1f, 1.f, 1.f, 1.f};
    Color constraintColor{1.f, 1.f, 1.f, 1.f};
};

} // namespace ox::physics
