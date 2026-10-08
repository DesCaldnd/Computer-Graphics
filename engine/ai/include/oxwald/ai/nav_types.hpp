#pragma once

#include <oxwald/core/types.hpp>

#include <glm/glm.hpp>

#include <array>
#include <functional>
#include <vector>

namespace ox::ai {

// Debug visualisation sink: line segment from → to with RGBA colour.
using DebugLineFn = std::function<void(glm::vec3, glm::vec3, glm::vec4)>;

// Recast area ids (0..63). 0 = not walkable. Areas select traversal cost (NavQueryFilter) and flags.
namespace NavArea {
inline constexpr u8 Null = 0;
inline constexpr u8 Ground = 1;
inline constexpr u8 Water = 2;
inline constexpr u8 Road = 3;
inline constexpr u8 Door = 4;
inline constexpr u8 Grass = 5;
inline constexpr u8 Jump = 6;
inline constexpr u8 Count = 64;
} // namespace NavArea

// Polygon ability flags; agents include/exclude them through their query filter.
namespace NavFlags {
inline constexpr u16 Walk = 0x01;
inline constexpr u16 Swim = 0x02;
inline constexpr u16 Door = 0x04;
inline constexpr u16 Jump = 0x08;
inline constexpr u16 Disabled = 0x10;
inline constexpr u16 All = 0xffff;
} // namespace NavFlags

using NavPolyRef = u64;

// Area id → polygon flags. Default: Ground/Road/Grass → Walk, Water → Swim, Door → Walk|Door, Jump → Jump.
std::array<u16, NavArea::Count> defaultAreaFlags();

// Query filter: per-area traversal cost multipliers and include/exclude flag masks.
struct NavQueryFilter {
    std::array<f32, NavArea::Count> areaCost = [] {
        std::array<f32, NavArea::Count> c{};
        c.fill(1.f);
        c[NavArea::Water] = 10.f;
        c[NavArea::Road] = 0.8f;
        c[NavArea::Grass] = 1.5f;
        c[NavArea::Jump] = 1.5f;
        return c;
    }();
    u16 includeFlags = NavFlags::All;
    u16 excludeFlags = NavFlags::Disabled;
};

struct OffMeshLink {
    glm::vec3 start{0.f};
    glm::vec3 end{0.f};
    f32 radius = 0.6f;
    bool bidirectional = true;
    u8 area = NavArea::Jump;
    u16 flags = 0; // 0 → derived from the area table
    u32 userId = 0;
};

// Marks the area of everything inside a vertical prism (e.g. water volumes, doors).
struct NavConvexVolume {
    std::vector<glm::vec3> points; // XZ polygon (y ignored)
    f32 minY = -1.f;
    f32 maxY = 1.f;
    u8 area = NavArea::Water;
};

// Triangle soup input. `triAreas` (optional, one per triangle) defaults to Ground.
struct NavMeshInput {
    std::vector<glm::vec3> vertices;
    std::vector<u32> indices;
    std::vector<u8> triAreas;
    std::vector<OffMeshLink> offMeshLinks;
    std::vector<NavConvexVolume> volumes;

    // Helpers for building test/procedural geometry (CCW seen from above = walkable up-facing).
    void addQuad(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& d, u8 area = NavArea::Ground);
    void addBox(const glm::vec3& bmin, const glm::vec3& bmax, u8 area = NavArea::Ground);
    void append(const NavMeshInput& other);
    [[nodiscard]] std::pair<glm::vec3, glm::vec3> bounds() const;
};

enum class NavPartition : u8 { Watershed, Monotone, Layers };

struct NavMeshBuildSettings {
    f32 cellSize = 0.3f;
    f32 cellHeight = 0.2f;
    f32 agentHeight = 2.0f;
    f32 agentRadius = 0.5f;
    f32 agentMaxClimb = 0.9f;
    f32 agentMaxSlope = 45.f; // degrees
    i32 regionMinSize = 8;    // cells² (side length)
    i32 regionMergeSize = 20;
    f32 edgeMaxLen = 12.f;    // world units
    f32 edgeMaxError = 1.3f;
    i32 vertsPerPoly = 6;
    f32 detailSampleDist = 6.f;     // in cells; < 0.9 disables detail sampling
    f32 detailSampleMaxError = 1.f; // in cell heights
    NavPartition partition = NavPartition::Watershed;
    bool filterLowHangingObstacles = true;
    bool filterLedgeSpans = true;
    bool filterWalkableLowHeightSpans = true;

    // Tiled build (large worlds / streaming / per-tile rebuilds).
    bool tiled = false;
    i32 tileSize = 48; // cells per tile side

    std::array<u16, NavArea::Count> areaFlags = defaultAreaFlags();
};

} // namespace ox::ai
