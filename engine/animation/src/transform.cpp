#include <oxwald/animation/transform.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <cmath>

namespace ox::anim {

Transform Transform::fromMatrix(const glm::mat4& m) {
    Transform t;
    t.translation = glm::vec3(m[3]);
    glm::vec3 c0(m[0]), c1(m[1]), c2(m[2]);
    t.scale = {glm::length(c0), glm::length(c1), glm::length(c2)};
    // Mirrored matrices: push the reflection into the X scale so the rotation stays proper.
    if (glm::dot(glm::cross(c0, c1), c2) < 0.0f) {
        t.scale.x = -t.scale.x;
    }
    glm::mat3 r(t.scale.x != 0.0f ? c0 / t.scale.x : glm::vec3(1, 0, 0),
                t.scale.y != 0.0f ? c1 / t.scale.y : glm::vec3(0, 1, 0),
                t.scale.z != 0.0f ? c2 / t.scale.z : glm::vec3(0, 0, 1));
    t.rotation = glm::normalize(glm::quat_cast(r));
    return t;
}

glm::mat4 Transform::toMatrix() const {
    glm::mat4 m = glm::mat4_cast(rotation);
    m[0] *= scale.x;
    m[1] *= scale.y;
    m[2] *= scale.z;
    m[3] = glm::vec4(translation, 1.0f);
    return m;
}

Transform Transform::inverse() const {
    Transform r;
    r.rotation = glm::conjugate(rotation);
    r.scale = glm::vec3(1.0f) / scale;
    r.translation = r.scale * (r.rotation * -translation);
    return r;
}

glm::quat nlerpShortest(const glm::quat& a, const glm::quat& b, f32 t) {
    const f32 sign = glm::dot(a, b) < 0.0f ? -1.0f : 1.0f;
    return glm::normalize(a * (1.0f - t) + (b * sign) * t);
}

glm::quat slerpShortest(const glm::quat& a, const glm::quat& b, f32 t) {
    glm::quat bb = b;
    f32 cosTheta = glm::dot(a, b);
    if (cosTheta < 0.0f) {
        bb = -b;
        cosTheta = -cosTheta;
    }
    if (cosTheta > 0.9995f) {
        return glm::normalize(a * (1.0f - t) + bb * t);
    }
    const f32 theta = std::acos(cosTheta);
    const f32 s = std::sin(theta);
    return (a * std::sin((1.0f - t) * theta) + bb * std::sin(t * theta)) / s;
}

glm::quat rotationBetween(const glm::vec3& from, const glm::vec3& to) {
    const f32 d = glm::dot(from, to);
    if (d < -0.999999f) {
        glm::vec3 axis = glm::cross(glm::vec3(1, 0, 0), from);
        if (glm::dot(axis, axis) < 1e-6f) {
            axis = glm::cross(glm::vec3(0, 1, 0), from);
        }
        return glm::angleAxis(glm::pi<f32>(), glm::normalize(axis));
    }
    const glm::vec3 c = glm::cross(from, to);
    return glm::normalize(glm::quat(1.0f + d, c.x, c.y, c.z));
}

Transform lerp(const Transform& a, const Transform& b, f32 t) {
    return {glm::mix(a.translation, b.translation, t), nlerpShortest(a.rotation, b.rotation, t),
            glm::mix(a.scale, b.scale, t)};
}

} // namespace ox::anim
