#pragma once

// CDLOD (Strugar 2010, "Continuous Distance-Dependent Level of Detail for Rendering Heightmaps").
// Chosen over geometry clipmaps because: per-node min/max bounds give tight frustum/occlusion culling,
// LOD transitions are continuous (vertex morphing, no T-junction stitching), one static grid mesh is
// instanced for every patch, and the node selection is cheap on the CPU (and portable to a GPU pass).
//
// Renderer contract (see docs/dev/modules/world.md):
//   * Draw TerrainGridMesh instanced, one instance per TerrainPatchGpu. Partial nodes (quadrantMask
//     != 0xF) draw only the quadrant index sub-ranges whose bit is set (group by quadrant).
//   * Vertex shader: world.xz = offset + uv*size; h = heightmap(world.xz); dist = |camera - (x,h,z)|;
//     morphK = cdlodMorphFactor(dist, morph); uv' = cdlodMorphVertex(uv, gridDim, morphK); re-sample h.
//     Skirt vertices (skirt = 1) are moved down by skirtDepth(lod).

#include <oxwald/world/common.hpp>
#include <oxwald/world/heightfield.hpp>

#include <array>
#include <vector>

namespace ox::world {

struct TerrainLodSettings {
    u32 leafNodeSize = 32;       // quads per side of a LOD-0 node == grid mesh dimension (even, >= 2)
    u32 lodCount = 6;            // LOD 0 = finest; node size doubles per level
    f32 viewDistance = 4000.f;   // visibility range of the coarsest LOD (metres); see crack-free note below
    f32 detailBalance = 2.f;     // range[i+1] / range[i] (>= 2 keeps neighbouring LODs within one level)
    f32 morphStartRatio = 0.66f; // morph region = last (1 - ratio) of each LOD band
    // Crack-free when range[l-1] + diagonal(node l-1) <= morphStart[l] for all l; with the defaults this
    // needs roughly viewDistance >= 1.2 * leafWorldSize * (2^lodCount - 1). TerrainQuadtree::build warns.
};

// Per-LOD distances. range[l] = farthest distance at which LOD l is used; morph[l] = (start, end) where
// vertices of LOD l morph completely into LOD l+1 geometry by end == range[l].
struct LodRanges {
    std::vector<f32> range;
    std::vector<glm::vec2> morph;
    static LodRanges compute(const TerrainLodSettings& s);
};

// One instanced draw of the terrain grid mesh.
struct TerrainPatch {
    glm::vec2 offset{0.f};    // world XZ of the node's minimum corner
    f32 size = 0.f;           // node size in metres (grid covers [offset, offset + size])
    u32 lod = 0;
    glm::vec2 morphRange{0.f}; // (start, end) distances for this LOD
    f32 minY = 0.f, maxY = 0.f;
    u8 quadrantMask = 0xF;    // bit q = draw quadrant q; q = (zHalf << 1) | xHalf; 0xF = whole node
};

// std430 layout (32 bytes) of TerrainPatch for the instance buffer.
struct TerrainPatchGpu {
    glm::vec4 offsetSizeLod; // xy = offset XZ, z = size, w = lod
    glm::vec4 morph;         // x = morphStart, y = morphEnd, z = 1/(end-start), w = quadrantMask
};
static_assert(sizeof(TerrainPatchGpu) == 32);
TerrainPatchGpu toGpu(const TerrainPatch& p);

struct TerrainSelection {
    std::vector<TerrainPatch> patches;
    std::array<u32, 16> patchesPerLod{};
    u32 visitedNodes = 0;
    bool truncated = false; // hit maxPatches
    void clear() {
        patches.clear();
        patchesPerLod.fill(0);
        visitedNodes = 0;
        truncated = false;
    }
};

struct TerrainSelectParams {
    glm::vec3 cameraPosition{0.f};
    Frustum frustum = Frustum::infinite();
    u32 maxPatches = 8192;
    // Optional second frustum-less pass (e.g. shadow cascades) can pass Frustum::infinite().
};

// Shader-identical helpers (mirror in the GPU terrain vertex shader).
f32 cdlodMorphFactor(f32 distance, glm::vec2 morphRange);
glm::vec2 cdlodMorphVertex(glm::vec2 uv, u32 gridDim, f32 morphK); // uv in [0,1] over the node

class TerrainQuadtree {
public:
    TerrainQuadtree() = default;
    TerrainQuadtree(const Heightfield& hf, const TerrainLodSettings& s);

    void build(const Heightfield& hf, const TerrainLodSettings& s);
    // Re-compute min/max of nodes overlapping a dirty rect (sample coords) after brush edits.
    void updateBounds(const Heightfield& hf, const IRect& dirty);

    void select(const TerrainSelectParams& params, TerrainSelection& out) const;

    [[nodiscard]] const LodRanges& ranges() const { return m_ranges; }
    [[nodiscard]] const TerrainLodSettings& settings() const { return m_settings; }
    [[nodiscard]] u32 nodeCountX(u32 lod) const { return m_levels[lod].countX; }
    [[nodiscard]] u32 nodeCountZ(u32 lod) const { return m_levels[lod].countZ; }
    [[nodiscard]] Aabb nodeBounds(u32 lod, u32 nx, u32 nz) const;
    [[nodiscard]] f32 nodeWorldSize(u32 lod) const { return f32(m_settings.leafNodeSize << lod) * m_spacing; }
    // Recommended skirt depth for a LOD (max height error when a vertex is morphed away).
    [[nodiscard]] f32 skirtDepth(u32 lod) const;
    void debugDraw(const TerrainSelection& sel, const DebugLineFn& line) const;

private:
    struct Level {
        u32 countX = 0, countZ = 0;
        std::vector<glm::vec2> minMax; // per node (minY, maxY), world metres
    };
    bool selectNode(u32 lod, u32 nx, u32 nz, const TerrainSelectParams& p, TerrainSelection& out) const;
    void computeLeaf(const Heightfield& hf, u32 nx, u32 nz);
    void propagate(u32 lod, u32 nx, u32 nz);

    TerrainLodSettings m_settings{};
    LodRanges m_ranges{};
    std::vector<Level> m_levels;
    glm::vec2 m_origin{0.f};
    f32 m_spacing = 1.f;
    u32 m_quads = 0; // resolution - 1
};

// --- grid mesh -------------------------------------------------------------------------------
struct TerrainGridVertex {
    f32 u = 0.f, v = 0.f; // [0,1] across the node
    f32 skirt = 0.f;      // 1 = skirt vertex (displace down by skirt depth)
};
struct IndexRange {
    u32 first = 0, count = 0;
};
struct TerrainGridMesh {
    u32 gridDim = 0;
    std::vector<TerrainGridVertex> vertices;
    std::vector<u32> indices;              // CCW seen from +Y; skirts face outwards
    std::array<IndexRange, 4> quadrants{}; // contiguous, in order 0..3; whole mesh = [0, indices.size())
};
TerrainGridMesh generateTerrainGrid(u32 gridDim, bool skirts = true);

// --- physics ---------------------------------------------------------------------------------
inline constexpr f32 kPhysicsHeightHole = 3.402823466e+38f; // == ox::physics::kHeightFieldHole

// Plain data matching ox::physics::ShapeDesc::heightField (see physics_bridge.hpp):
// world position of sample (x,z) = offset + scale * (x, heights[z*sampleCount + x], z).
struct PhysicsHeightfieldTile {
    i32 tileX = 0, tileZ = 0;
    u32 sampleCount = 0;
    std::vector<f32> heights; // world metres; kPhysicsHeightHole for holes / outside the map
    glm::vec3 offset{0.f};
    glm::vec3 scale{1.f};
};

// Tiles of tileQuads x tileQuads quads (tileQuads + 1 samples, edges shared with neighbours).
PhysicsHeightfieldTile buildPhysicsTile(const Heightfield& hf, i32 tileX, i32 tileZ, u32 tileQuads);
std::vector<PhysicsHeightfieldTile> buildPhysicsTiles(const Heightfield& hf, u32 tileQuads);
// Tile coordinates whose samples overlap a dirty rect (rebuild these after editing).
std::vector<glm::ivec2> physicsTilesOverlapping(const IRect& dirty, u32 tileQuads, u32 heightfieldResolution);

} // namespace ox::world
