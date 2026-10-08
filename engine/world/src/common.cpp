#include <oxwald/world/common.hpp>

#include <glm/geometric.hpp>

#include <algorithm>

namespace ox::world {

IRect IRect::merged(const IRect& o) const {
    if (empty()) {
        return o;
    }
    if (o.empty()) {
        return *this;
    }
    return {std::min(x0, o.x0), std::min(z0, o.z0), std::max(x1, o.x1), std::max(z1, o.z1)};
}

IRect IRect::intersected(const IRect& o) const {
    IRect r{std::max(x0, o.x0), std::max(z0, o.z0), std::min(x1, o.x1), std::min(z1, o.z1)};
    if (r.empty()) {
        return {};
    }
    return r;
}

f32 distanceSq(const Aabb& box, const glm::vec3& p) {
    const glm::vec3 q = glm::clamp(p, box.min, box.max);
    const glm::vec3 d = p - q;
    return glm::dot(d, d);
}

Frustum Frustum::fromViewProjection(const glm::mat4& m) {
    auto row = [&](int i) { return glm::vec4(m[0][i], m[1][i], m[2][i], m[3][i]); };
    const glm::vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
    Frustum f;
    f.planes = {r3 + r0, r3 - r0, r3 + r1, r3 - r1, r2, r3 - r2};
    for (auto& p : f.planes) {
        const f32 len = glm::length(glm::vec3(p));
        if (len < 1e-8f) {
            p = glm::vec4(0.f, 0.f, 0.f, 1.f); // infinite far plane (reversed-Z infinite projection)
        } else {
            p /= len;
        }
    }
    return f;
}

Frustum Frustum::infinite() {
    Frustum f;
    f.planes.fill(glm::vec4(0.f, 0.f, 0.f, 1.f));
    return f;
}

bool Frustum::intersects(const Aabb& box) const {
    for (const auto& p : planes) {
        const glm::vec3 n(p);
        const glm::vec3 pv{n.x >= 0.f ? box.max.x : box.min.x, n.y >= 0.f ? box.max.y : box.min.y,
                           n.z >= 0.f ? box.max.z : box.min.z};
        if (glm::dot(n, pv) + p.w < 0.f) {
            return false;
        }
    }
    return true;
}

bool Frustum::intersectsSphere(const glm::vec3& c, f32 r) const {
    for (const auto& p : planes) {
        if (glm::dot(glm::vec3(p), c) + p.w < -r) {
            return false;
        }
    }
    return true;
}

} // namespace ox::world
