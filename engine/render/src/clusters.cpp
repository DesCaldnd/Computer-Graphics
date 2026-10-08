#include <oxwald/render/clusters.hpp>

#include <algorithm>
#include <cmath>

namespace ox::render {

f32 ClusterGrid::sliceScale() const { return f32(z) / std::log(farPlane / nearPlane); }
f32 ClusterGrid::sliceBias() const { return -f32(z) * std::log(nearPlane) / std::log(farPlane / nearPlane); }

u32 ClusterGrid::slice(f32 viewDepth) const {
    const f32 s = std::log(std::max(viewDepth, 1e-4f)) * sliceScale() + sliceBias();
    return u32(std::clamp(s, 0.0f, f32(z - 1)));
}

f32 ClusterGrid::sliceNear(u32 s) const { return nearPlane * std::pow(farPlane / nearPlane, f32(s) / f32(z)); }

u32 ClusterGrid::clusterIndex(glm::vec2 uv, f32 viewDepth) const {
    const u32 tx = std::min(u32(std::max(uv.x, 0.0f) * f32(x)), x - 1);
    const u32 ty = std::min(u32(std::max(uv.y, 0.0f) * f32(y)), y - 1);
    return tx + ty * x + slice(viewDepth) * x * y;
}

AABB ClusterGrid::clusterBounds(u32 cx, u32 cy, u32 cz, const glm::mat4& invProj) const {
    const f32 d0 = sliceNear(cz), d1 = sliceNear(cz + 1);
    const glm::vec2 ndcMin = glm::vec2(f32(cx) / f32(x), f32(cy) / f32(y)) * 2.0f - 1.0f;
    const glm::vec2 ndcMax = glm::vec2(f32(cx + 1) / f32(x), f32(cy + 1) / f32(y)) * 2.0f - 1.0f;
    AABB box;
    for (u32 i = 0; i < 4; ++i) {
        const glm::vec2 ndc{(i & 1) ? ndcMax.x : ndcMin.x, (i & 2) ? ndcMax.y : ndcMin.y};
        glm::vec4 p = invProj * glm::vec4(ndc, 1.0f, 1.0f);
        const glm::vec3 v = glm::vec3(p) / p.w;
        box.expand(v * (d0 / -v.z));
        box.expand(v * (d1 / -v.z));
    }
    return box;
}

bool sphereIntersectsAabb(const glm::vec3& center, f32 radius, const AABB& box) {
    const glm::vec3 c = glm::clamp(center, box.min, box.max);
    const glm::vec3 d = center - c;
    return glm::dot(d, d) <= radius * radius;
}

} // namespace ox::render
