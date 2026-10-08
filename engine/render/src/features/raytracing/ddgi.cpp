// DDGI volume layout helpers (CPU mirror of raytracing/ddgi.glsl).
#include <oxwald/render/features/raytracing/ddgi.hpp>

#include <cmath>

namespace ox::render::rt {

namespace {
i32 positiveMod(i32 a, i32 m) {
    const i32 r = a % m;
    return r < 0 ? r + m : r;
}
} // namespace

glm::ivec3 ddgiMinCoord(const glm::ivec3& counts, f32 spacing, const glm::vec3& cameraPosition) {
    const glm::ivec3 cam = glm::ivec3(glm::floor(cameraPosition / spacing));
    return cam - counts / 2;
}

glm::vec3 ddgiProbePosition(const glm::ivec3& worldCoord, f32 spacing) {
    return (glm::vec3(worldCoord) + 0.5f) * spacing;
}

u32 ddgiStorageIndex(const glm::ivec3& c, const glm::ivec3& counts) {
    const i32 x = positiveMod(c.x, counts.x), y = positiveMod(c.y, counts.y), z = positiveMod(c.z, counts.z);
    return u32(x + y * counts.x + z * counts.x * counts.y);
}

glm::ivec3 ddgiWorldCoord(u32 s, const glm::ivec3& minCoord, const glm::ivec3& counts) {
    const glm::ivec3 slot{i32(s) % counts.x, (i32(s) / counts.x) % counts.y, i32(s) / (counts.x * counts.y)};
    // The world coordinate inside [minCoord, minCoord + counts) whose modulo equals the slot.
    glm::ivec3 c;
    for (int a = 0; a < 3; ++a) {
        const i32 base = minCoord[a];
        c[a] = base + positiveMod(slot[a] - base, counts[a]);
    }
    return c;
}

glm::uvec2 ddgiAtlasSize(const glm::ivec3& counts, u32 texels) {
    const u32 tile = texels + 2;
    return {u32(counts.x * counts.y) * tile, u32(counts.z) * tile};
}

glm::uvec2 ddgiTileInterior(u32 s, const glm::ivec3& counts, u32 texels) {
    const u32 perRow = u32(counts.x * counts.y);
    const u32 tile = texels + 2;
    return {(s % perRow) * tile + 1, (s / perRow) * tile + 1};
}

glm::vec3 ddgiRayDirection(u32 i, u32 n) {
    // Spherical Fibonacci (Keinert et al. 2015).
    const f32 golden = 1.61803398875f;
    const f32 phi = 2.0f * kPi * std::fmod(f32(i) / golden, 1.0f);
    const f32 cosTheta = 1.0f - (2.0f * f32(i) + 1.0f) / f32(n);
    const f32 sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
    return {std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta};
}

} // namespace ox::render::rt
