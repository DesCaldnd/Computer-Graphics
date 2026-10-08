#pragma once

#include <oxwald/world/common.hpp>
#include <oxwald/world/heightfield.hpp>
#include <oxwald/world/splat_map.hpp>

#include <glm/gtc/quaternion.hpp>

#include <functional>
#include <span>
#include <string>
#include <vector>

namespace ox::world {

// --- Poisson disk / blue noise ---------------------------------------------------------------
struct PoissonDisk {
    // Bridson's algorithm in [0,size.x) x [0,size.y); every pair is >= minDistance apart.
    static std::vector<glm::vec2> generate(glm::vec2 size, f32 minDistance, u32 seed, u32 attempts = 30);
    // Toroidal (wrap-around) pattern in [0,period)^2: tiling it keeps the minimum distance across tile
    // borders, so chunks can be scattered independently and still be seamless.
    static std::vector<glm::vec2> generateTileable(f32 period, f32 minDistance, u32 seed, u32 attempts = 30);
};

// --- layers & rules --------------------------------------------------------------------------
enum class VegetationKind : u8 { Grass, Detail, Tree };
enum class VegetationColliderShape : u8 { Capsule, Cylinder, Box };

struct VegetationLodSettings {
    f32 lodDistances[2] = {25.f, 60.f};        // end of mesh LOD 0 and 1; LOD 2 until impostorDistance
    f32 impostorDistance = 200.f;              // switch to billboard/octahedral impostor (<= 0: never)
    f32 cullDistance = 600.f;
    f32 fadeRange = 10.f;                      // dithered cross-fade width at each switch
};

struct VegetationLayer {
    std::string name;
    VegetationKind kind = VegetationKind::Grass;
    u16 prototype = 0;          // renderer mesh/material index
    u32 seed = 1;
    // Placement
    f32 minDistance = 1.f;      // Poisson radius (metres)
    f32 density = 1.f;          // acceptance probability multiplier [0,1]
    f32 minHeight = -1e9f, maxHeight = 1e9f; // world metres
    f32 minSlopeDeg = 0.f, maxSlopeDeg = 35.f;
    i32 splatLayer = -1;        // >= 0: require this splat layer
    f32 minSplatWeight = 0.3f;  // below → rejected; otherwise acceptance *= weight
    i32 densityMapIndex = -1;   // index into ScatterContext::densityMaps
    // Per-instance variation
    f32 minScale = 0.8f, maxScale = 1.2f;
    bool randomYaw = true;
    f32 alignToNormal = 0.f;    // 0 = upright, 1 = terrain normal
    f32 sinkDepth = 0.f;        // metres (× scale) pushed into the ground (hide roots on slopes)
    glm::vec4 tintA{1.f}, tintB{1.f}; // random lerp, linear RGBA
    f32 boundingRadius = 1.f;   // unscaled, metres (GPU culling)
    VegetationLodSettings lod{};
    // Physics (trees)
    bool collider = false;
    VegetationColliderShape colliderShape = VegetationColliderShape::Capsule;
    f32 colliderRadius = 0.3f, colliderHalfHeight = 2.f; // unscaled; capsule half height excludes caps
};

// Scalar density over a world rect, bilinear; values in [0,1].
struct DensityMap {
    u32 resolution = 0;
    glm::vec2 origin{0.f};
    f32 worldSize = 0.f;
    std::vector<f32> values;
    [[nodiscard]] f32 sample(glm::vec2 worldXZ) const;
};

struct ExclusionZone {
    enum class Shape : u8 { Circle, Rect };
    Shape shape = Shape::Circle;
    glm::vec2 center{0.f};
    glm::vec2 halfExtents{1.f}; // Circle: x = radius
    u32 layerMask = ~0u;        // bit i = excludes layer i
    [[nodiscard]] bool contains(glm::vec2 p) const;
};

struct ScatterContext {
    const Heightfield* heightfield = nullptr; // required
    const SplatMap* splat = nullptr;
    std::span<const DensityMap> densityMaps;
    std::span<const ExclusionZone> exclusions;
    std::function<f32(glm::vec2 worldXZ, u32 layerIndex)> customDensity; // optional extra multiplier
};

// --- instances -------------------------------------------------------------------------------
struct VegetationInstance {
    glm::vec3 position{0.f};
    glm::quat rotation{1.f, 0.f, 0.f, 0.f};
    f32 scale = 1.f;
    u32 tint = 0xFFFFFFFFu; // RGBA8 (r in low byte)
    f32 random = 0.f;       // [0,1)
    u16 layer = 0;
    u16 prototype = 0;
    bool operator==(const VegetationInstance&) const = default;
};

// GPU instance layout — 64 bytes, std430/scalar compatible:
//   transform: rows of the affine object→world 3x4 matrix (same layout as VkTransformMatrixKHR, so it
//              can feed ray tracing instances directly). GLSL: `mat3x4 m = mat3x4(t[0], t[1], t[2]);
//              vec3 world = vec4(local, 1.0) * m;`
//   tint:      packUnorm4x8 RGBA (linear)
//   random:    [0,1) per instance (wind phase, colour/size variation)
//   prototypeLayerFlags: bits 0-15 prototype, 16-23 layer, 24-31 flags (kVegFlag*)
//   boundingRadius: world-space radius around the origin for GPU culling
struct VegetationInstanceGpu {
    glm::vec4 transform[3];
    u32 tint;
    f32 random;
    u32 prototypeLayerFlags;
    f32 boundingRadius;
};
static_assert(sizeof(VegetationInstanceGpu) == 64);
inline constexpr u32 kVegFlagTree = 1u << 24;
inline constexpr u32 kVegFlagCastsShadow = 1u << 25;

VegetationInstanceGpu toGpu(const VegetationInstance& inst, const VegetationLayer& layer);
glm::mat4 instanceMatrix(const VegetationInstance& inst);
u32 packRgba8(glm::vec4 c);
glm::vec4 unpackRgba8(u32 c);

// Collider descriptor (trees). Pose of the physics body; shape in local space (see physics_bridge.hpp).
struct VegetationCollider {
    glm::vec3 position{0.f};
    glm::quat rotation{1.f, 0.f, 0.f, 0.f};
    VegetationColliderShape shape = VegetationColliderShape::Capsule;
    f32 radius = 0.3f;
    f32 halfHeight = 1.f;
    u32 instanceIndex = 0;
    u16 layer = 0;
};

// Instances of one chunk, grouped into cells (per layer) for culling.
struct VegetationCell {
    Aabb bounds;
    u32 first = 0, count = 0; // range into VegetationChunk::instances
    u16 layer = 0;
};

struct VegetationChunk {
    glm::vec2 origin{0.f};
    f32 size = 0.f;
    std::vector<VegetationInstance> instances; // sorted by (cell, layer) after buildCells
    std::vector<VegetationCollider> colliders;
    std::vector<VegetationCell> cells;
    void buildCells(std::span<const VegetationLayer> layers, f32 cellSize);
};

class VegetationScatterer {
public:
    // patternPeriod: size of the tileable Poisson pattern per layer (rounded up to >= 8 * minDistance).
    explicit VegetationScatterer(std::vector<VegetationLayer> layers, f32 patternPeriod = 64.f);

    [[nodiscard]] const std::vector<VegetationLayer>& layers() const { return m_layers; }
    // Deterministic, seamless across chunk borders (same result regardless of chunking).
    // Instances are placed in [origin, origin+size) (half-open), with cells of cellSize built.
    [[nodiscard]] VegetationChunk scatter(glm::vec2 origin, f32 size, const ScatterContext& ctx, f32 cellSize = 32.f) const;
    // Rule evaluation for one point (exposed for tools/tests): acceptance probability in [0,1].
    [[nodiscard]] f32 acceptance(u32 layerIndex, glm::vec2 worldXZ, const ScatterContext& ctx) const;

private:
    struct Pattern {
        f32 period = 0.f;
        std::vector<glm::vec2> points;
    };
    std::vector<VegetationLayer> m_layers;
    std::vector<Pattern> m_patterns;
};

// --- LOD / culling ---------------------------------------------------------------------------
struct VegetationLodResult {
    u8 lod = 0;           // mesh LOD 0..2, kImpostor or kCulled
    f32 fade = 1.f;       // 1 = fully this LOD; < 1 = cross-fading into the next (dither)
    static constexpr u8 kImpostor = 3;
    static constexpr u8 kCulled = 0xFF;
};
VegetationLodResult selectVegetationLod(const VegetationLodSettings& s, f32 distance);

struct VisibleVegetationCell {
    u32 cell = 0;
    f32 distance = 0.f;        // camera to cell bounds
    VegetationLodResult lod{}; // conservative (nearest point of the cell); per-instance LOD on GPU
};
// Frustum + distance culling of a chunk's cells.
void cullVegetationCells(const VegetationChunk& chunk, std::span<const VegetationLayer> layers, const Frustum& frustum,
                         glm::vec3 cameraPos, std::vector<VisibleVegetationCell>& out, f32 distanceScale = 1.f);

void debugDrawVegetationCells(const VegetationChunk& chunk, const DebugLineFn& line);

} // namespace ox::world
