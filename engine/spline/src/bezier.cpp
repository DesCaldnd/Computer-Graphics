#include <oxwald/spline/bezier.hpp>

#include <glm/geometric.hpp>

namespace ox::spline {

glm::vec3 bezierPosition(const CubicBezier& b, f32 u) {
    const f32 v = 1.0f - u;
    return (v * v * v) * b[0] + (3.0f * v * v * u) * b[1] + (3.0f * v * u * u) * b[2] + (u * u * u) * b[3];
}

glm::vec3 bezierDerivative(const CubicBezier& b, f32 u) {
    const f32 v = 1.0f - u;
    return (3.0f * v * v) * (b[1] - b[0]) + (6.0f * v * u) * (b[2] - b[1]) + (3.0f * u * u) * (b[3] - b[2]);
}

glm::vec3 bezierSecondDerivative(const CubicBezier& b, f32 u) {
    const f32 v = 1.0f - u;
    return (6.0f * v) * (b[2] - 2.0f * b[1] + b[0]) + (6.0f * u) * (b[3] - 2.0f * b[2] + b[1]);
}

glm::vec3 deCasteljau(const CubicBezier& b, f32 u) {
    glm::vec3 p[4] = {b[0], b[1], b[2], b[3]};
    for (int level = 3; level > 0; --level) {
        for (int i = 0; i < level; ++i) {
            p[i] = glm::mix(p[i], p[i + 1], u);
        }
    }
    return p[0];
}

std::pair<CubicBezier, CubicBezier> splitBezier(const CubicBezier& b, f32 u) {
    const glm::vec3 p01 = glm::mix(b[0], b[1], u);
    const glm::vec3 p12 = glm::mix(b[1], b[2], u);
    const glm::vec3 p23 = glm::mix(b[2], b[3], u);
    const glm::vec3 p012 = glm::mix(p01, p12, u);
    const glm::vec3 p123 = glm::mix(p12, p23, u);
    const glm::vec3 mid = glm::mix(p012, p123, u);
    return {CubicBezier{b[0], p01, p012, mid}, CubicBezier{mid, p123, p23, b[3]}};
}

CubicBezier hermiteToBezier(const glm::vec3& p0, const glm::vec3& m0, const glm::vec3& p1, const glm::vec3& m1) {
    return CubicBezier{p0, p0 + m0 / 3.0f, p1 - m1 / 3.0f, p1};
}

} // namespace ox::spline
