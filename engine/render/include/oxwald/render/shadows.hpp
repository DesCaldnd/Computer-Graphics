#pragma once

// CPU side of the raster shadow system (pure math, unit tested): cascade splits + stabilised cascade matrices,
// shadow atlas allocation by importance, point light face matrices. Rendering lives in the ShadowsRaster feature.

#include <oxwald/core/math.hpp>
#include <oxwald/render/gpu_types.hpp>

#include <array>
#include <optional>
#include <span>
#include <vector>

namespace ox::render {

// Practical split scheme (Zhang et al.): split_i = lerp(uniform_i, log_i, lambda). Returns `count` far distances.
[[nodiscard]] std::vector<f32> cascadeSplits(f32 nearPlane, f32 shadowDistance, u32 count, f32 lambda);

struct CascadeInput {
    glm::mat4 cameraWorld{1.0f}; // camera to world
    f32 verticalFov = glm::radians(60.0f);
    f32 aspect = 1.0f;
    bool orthographic = false;
    f32 orthographicHeight = 10.0f;
    f32 nearPlane = 0.1f;
    f32 shadowDistance = 100.0f;
    u32 cascadeCount = 4;
    f32 lambda = 0.75f;
    glm::vec3 lightDirection{0.0f, -1.0f, 0.0f}; // direction the light travels
    u32 resolution = 2048;
    f32 casterExtension = 200.0f; // pull the near plane towards the light to catch off-screen casters
};

struct Cascade {
    glm::mat4 view{1.0f};
    glm::mat4 proj{1.0f};
    glm::mat4 viewProj{1.0f}; // reversed-Z ortho, texel snapped
    f32 splitNear = 0.0f;
    f32 splitFar = 0.0f;
    f32 radius = 0.0f;     // bounding sphere radius of the cascade slice (stable under rotation)
    f32 texelWorld = 0.0f; // world size of one texel
    f32 depthRange = 0.0f; // world distance mapped to depth [0,1]
};

// Bounding-sphere fitted cascades (rotation invariant size) with the light-space origin snapped to whole texels,
// so sub-texel camera motion does not change the shadow map (no shimmering).
[[nodiscard]] std::vector<Cascade> computeCascades(const CascadeInput& input);

// Point light face orientation (shared with shadows.glsl: OX_CUBE_FORWARD/OX_CUBE_UP tables).
[[nodiscard]] glm::vec3 cubeFaceForward(u32 face);
[[nodiscard]] glm::vec3 cubeFaceUp(u32 face);
// Field of view of a face including a guard band of `guardTexels` so PCF kernels stay inside the face.
[[nodiscard]] f32 cubeFaceFov(u32 resolution, f32 guardTexels);
[[nodiscard]] glm::mat4 cubeFaceViewProj(glm::vec3 position, u32 face, f32 fov, f32 nearPlane, f32 farPlane);
[[nodiscard]] glm::mat4 spotViewProj(glm::vec3 position, glm::vec3 direction, f32 outerConeDegrees, f32 nearPlane,
                                     f32 farPlane);

// Quadtree allocator for square power-of-two tiles in a square atlas.
class ShadowAtlasAllocator {
public:
    struct Tile {
        u32 x = 0, y = 0, size = 0;
        bool operator==(const Tile&) const = default;
    };

    explicit ShadowAtlasAllocator(u32 atlasSize = 4096, u32 minTileSize = 64);
    void reset(u32 atlasSize, u32 minTileSize = 64);
    void clear();
    [[nodiscard]] std::optional<Tile> allocate(u32 size); // size rounded up to a power of two
    void free(const Tile& tile);
    [[nodiscard]] u32 atlasSize() const { return m_size; }
    [[nodiscard]] u64 usedTexels() const { return m_used; }

private:
    bool allocateIn(u32 node, u32 nx, u32 ny, u32 nsize, u32 want, Tile& out);
    bool freeIn(u32 node, u32 nx, u32 ny, u32 nsize, const Tile& t);
    struct Node {
        u8 state = 0; // 0 free, 1 full (allocated tile), 2 split
        u32 children = 0; // index of the first of 4 children
    };
    std::vector<Node> m_nodes;
    u32 m_size = 0;
    u32 m_minTile = 64;
    u64 m_used = 0;
};

// Shadow request for a local light, ranked by importance.
struct ShadowRequest {
    u32 lightIndex = 0;   // caller's index
    glm::vec3 position{0.0f};
    f32 range = 1.0f;
    bool point = false;
    f32 priority = 1.0f;  // user priority multiplier (e.g. intensity-based)
    u32 resolutionHint = 0; // LightComponent::shadowResolution (0 = auto)
};

struct ShadowAllocation {
    u32 lightIndex = 0;
    bool point = false;
    f32 importance = 0.0f;
    u32 resolution = 0;               // tile size (spot) or face size (point)
    ShadowAtlasAllocator::Tile tile;  // spot
    u32 cubeSlot = 0;                 // point
};

struct ShadowBudget {
    u32 atlasSize = 4096;
    u32 minResolution = 128;
    u32 maxResolution = 1024;
    u32 maxShadowedLights = 16;
    u32 maxPointLights = 8;
    u32 pointResolution = 512;
};

// Importance = projected size of the light's sphere of influence on screen × priority. Spot tiles get
// resolution ∝ importance (power of two, clamped); when the atlas is full lower-importance lights are downsized or
// dropped. Points take a cube slot (fixed face size) in importance order.
[[nodiscard]] f32 shadowImportance(const ShadowRequest& r, const glm::vec3& cameraPos, f32 cameraFovY,
                                   f32 viewportHeight);
[[nodiscard]] std::vector<ShadowAllocation> allocateShadows(std::span<const ShadowRequest> requests,
                                                            const glm::vec3& cameraPos, f32 cameraFovY,
                                                            f32 viewportHeight, const ShadowBudget& budget,
                                                            ShadowAtlasAllocator& atlas);

} // namespace ox::render
