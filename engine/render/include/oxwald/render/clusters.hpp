#pragma once

// Clustered light culling math, mirrored by engine/shaders/render/common/clusters.glsl.
// Grid: X × Y screen tiles × Z exponential (log) depth slices between near and far:
//   slice(z) = floor(log(z) · scale + bias),  scale = Z / log(far / near),  bias = -Z · log(near) / log(far / near)
// Cluster index = x + y·X + slice·X·Y. LightClusters buffer: [counts: u32 × N][indices: u32 × N × maxPerCluster].

#include <oxwald/core/math.hpp>

namespace ox::render {

struct ClusterGrid {
    u32 x = 16, y = 9, z = 24;
    u32 maxLightsPerCluster = 256;
    f32 nearPlane = 0.1f;
    f32 farPlane = 500.0f;

    [[nodiscard]] u32 clusterCount() const { return x * y * z; }
    [[nodiscard]] f32 sliceScale() const;
    [[nodiscard]] f32 sliceBias() const;
    // View-space distance (positive, along -Z) → slice, clamped to [0, z-1].
    [[nodiscard]] u32 slice(f32 viewDepth) const;
    // Near/far view depth of a slice.
    [[nodiscard]] f32 sliceNear(u32 s) const;
    [[nodiscard]] f32 sliceFar(u32 s) const { return sliceNear(s + 1); }
    // uv in [0,1]² (top-left origin) + view depth → cluster index.
    [[nodiscard]] u32 clusterIndex(glm::vec2 uv, f32 viewDepth) const;
    // Size in bytes of the LightClusters buffer.
    [[nodiscard]] u64 bufferSize() const { return u64(clusterCount()) * (1 + maxLightsPerCluster) * 4; }
    // View-space AABB of a cluster for a projection (invProj maps NDC → view, Vulkan Y flip, reversed-Z).
    [[nodiscard]] AABB clusterBounds(u32 cx, u32 cy, u32 cz, const glm::mat4& invProj) const;
};

// CPU reference of the culling test (sphere vs cluster AABB), used by tests.
[[nodiscard]] bool sphereIntersectsAabb(const glm::vec3& center, f32 radius, const AABB& box);

} // namespace ox::render
