#pragma once

#include <oxwald/core/types.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace ox::anim {

// Translation / rotation / scale. Composition ignores shear (non-uniform scale under rotation),
// which is the usual game-animation convention.
struct Transform {
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};

    static Transform identity() { return {}; }
    static Transform fromMatrix(const glm::mat4& m);

    glm::mat4 toMatrix() const;
    glm::vec3 transformPoint(const glm::vec3& p) const { return translation + rotation * (scale * p); }
    glm::vec3 transformVector(const glm::vec3& v) const { return rotation * (scale * v); }
    Transform inverse() const;

    // parent * child: child expressed in parent space → result in parent's parent space.
    friend Transform operator*(const Transform& parent, const Transform& child) {
        Transform r;
        r.translation = parent.transformPoint(child.translation);
        r.rotation = glm::normalize(parent.rotation * child.rotation);
        r.scale = parent.scale * child.scale;
        return r;
    }
};

// Quaternion helpers used across the module.
// Normalised lerp along the shortest arc.
glm::quat nlerpShortest(const glm::quat& a, const glm::quat& b, f32 t);
// Spherical lerp along the shortest arc.
glm::quat slerpShortest(const glm::quat& a, const glm::quat& b, f32 t);
// Rotation that maps unit vector `from` to unit vector `to` (handles the antiparallel case).
glm::quat rotationBetween(const glm::vec3& from, const glm::vec3& to);

Transform lerp(const Transform& a, const Transform& b, f32 t);

} // namespace ox::anim
