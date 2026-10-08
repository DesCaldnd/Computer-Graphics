#pragma once

#include <oxwald/ai/nav_types.hpp>

#include <memory>
#include <optional>
#include <span>
#include <vector>

class dtNavMesh;

namespace ox::ai {

struct NavMeshStats {
    u32 tiles = 0;
    u32 polygons = 0;
    u32 vertices = 0;
    u32 offMeshLinks = 0;
    u32 dataBytes = 0;
};

// Detour navigation mesh (solo = a single tile, or tiled). Built from triangle soup with Recast.
class NavMesh {
public:
    ~NavMesh();
    NavMesh(const NavMesh&) = delete;
    NavMesh& operator=(const NavMesh&) = delete;

    // Builds solo or tiled according to settings.tiled. Returns null (and logs) on failure.
    static std::unique_ptr<NavMesh> build(const NavMeshInput& input, const NavMeshBuildSettings& settings);

    // Tiled meshes only: replaces the stored input and rebuilds every tile overlapping [bmin, bmax]
    // (e.g. the bounds of a newly placed/removed obstacle). Returns the number of rebuilt tiles.
    i32 rebuildTiles(const NavMeshInput& newInput, const glm::vec3& bmin, const glm::vec3& bmax);
    bool rebuildTile(i32 tx, i32 ty);
    void removeTile(i32 tx, i32 ty);
    [[nodiscard]] glm::ivec2 tileCoord(const glm::vec3& pos) const;
    [[nodiscard]] glm::ivec2 tileGridSize() const;

    // Binary blob: header + build settings + every tile's Detour data. Tiled meshes keep no geometry after
    // loading, so rebuildTiles() requires calling it with input again.
    [[nodiscard]] std::vector<u8> serialize() const;
    static std::unique_ptr<NavMesh> deserialize(std::span<const u8> bytes);

    [[nodiscard]] NavMeshStats stats() const;
    [[nodiscard]] const NavMeshBuildSettings& settings() const { return m_settings; }
    [[nodiscard]] bool tiled() const { return m_settings.tiled; }
    [[nodiscard]] std::pair<glm::vec3, glm::vec3> bounds() const { return {m_bmin, m_bmax}; }

    // Polygon flags at runtime (e.g. close a door: setPolyFlags(ref, NavFlags::Disabled)).
    bool setPolyFlags(NavPolyRef ref, u16 flags);
    [[nodiscard]] u16 polyFlags(NavPolyRef ref) const;
    [[nodiscard]] u8 polyArea(NavPolyRef ref) const;

    // Polygon outlines (cyan), off-mesh links (magenta). Optional per-area tint.
    void debugDraw(const DebugLineFn& line) const;

    [[nodiscard]] dtNavMesh* detour() const { return m_mesh; }

private:
    friend class NavTileCache;
    NavMesh() = default;
    bool initTiled(const NavMeshInput& input, const NavMeshBuildSettings& settings);
    bool initSolo(const NavMeshInput& input, const NavMeshBuildSettings& settings);

    dtNavMesh* m_mesh = nullptr;
    NavMeshBuildSettings m_settings;
    NavMeshInput m_input; // kept for tile rebuilds
    glm::vec3 m_bmin{0.f}, m_bmax{0.f};
    glm::ivec2 m_tiles{1, 1};
};

} // namespace ox::ai
