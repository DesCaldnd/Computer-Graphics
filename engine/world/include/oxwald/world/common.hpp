#pragma once

#include <oxwald/core/types.hpp>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <functional>

namespace ox::world {

// Debug visualisation sink: (from, to, rgba). Hook it to ox::DebugDraw or any line renderer.
using DebugLineFn = std::function<void(glm::vec3, glm::vec3, glm::vec4)>;

// Integer rectangle in sample/texel space, half-open: [x0, x1) x [z0, z1). Used as "dirty rect" for
// partial GPU texture uploads and physics tile rebuilds.
struct IRect {
    i32 x0 = 0, z0 = 0, x1 = 0, z1 = 0;

    [[nodiscard]] bool empty() const { return x1 <= x0 || z1 <= z0; }
    [[nodiscard]] i32 width() const { return x1 - x0; }
    [[nodiscard]] i32 height() const { return z1 - z0; }
    [[nodiscard]] bool contains(i32 x, i32 z) const { return x >= x0 && x < x1 && z >= z0 && z < z1; }
    [[nodiscard]] IRect merged(const IRect& o) const;
    [[nodiscard]] IRect intersected(const IRect& o) const;
    bool operator==(const IRect&) const = default;
};

struct Aabb {
    glm::vec3 min{0.f};
    glm::vec3 max{0.f};
    [[nodiscard]] glm::vec3 center() const { return (min + max) * 0.5f; }
    [[nodiscard]] glm::vec3 extents() const { return (max - min) * 0.5f; }
};

// Squared distance from a point to an AABB (0 inside).
f32 distanceSq(const Aabb& box, const glm::vec3& p);

// View frustum as 6 inward-facing planes (xyz = normal, w = d; inside when dot(n,p) + d >= 0).
// Built from a GLM view-projection with depth range [0,1]; works for standard and reversed-Z, and
// for infinite far planes (a degenerate plane is replaced by an always-pass plane).
struct Frustum {
    std::array<glm::vec4, 6> planes{};

    static Frustum fromViewProjection(const glm::mat4& viewProj);
    // Everything passes (useful for tests / shadow passes that cull elsewhere).
    static Frustum infinite();

    [[nodiscard]] bool intersects(const Aabb& box) const;
    [[nodiscard]] bool intersectsSphere(const glm::vec3& c, f32 r) const;
};

} // namespace ox::world
