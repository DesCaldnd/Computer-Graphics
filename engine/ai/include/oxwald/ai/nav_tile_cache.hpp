#pragma once

#include <oxwald/ai/navmesh.hpp>

#include <memory>

class dtTileCache;

namespace ox::ai {

using NavObstacleId = u32; // 0 = invalid

struct NavTileCacheSettings {
    NavMeshBuildSettings build; // tiled/partition are forced (layers)
    i32 maxObstacles = 256;
    i32 expectedLayersPerTile = 4;
};

// Dynamic obstacles via DetourTileCache: compressed heightfield layers are kept per tile, so adding or
// removing a cylinder/box obstacle only re-triangulates the affected tiles (cheap, can run every frame).
class NavTileCache {
public:
    ~NavTileCache();
    NavTileCache(const NavTileCache&) = delete;
    NavTileCache& operator=(const NavTileCache&) = delete;

    static std::unique_ptr<NavTileCache> build(const NavMeshInput& input, const NavTileCacheSettings& settings);

    NavObstacleId addCylinder(const glm::vec3& basePosition, f32 radius, f32 height);
    NavObstacleId addBox(const glm::vec3& bmin, const glm::vec3& bmax);
    NavObstacleId addOrientedBox(const glm::vec3& center, const glm::vec3& halfExtents, f32 yawRadians);
    bool removeObstacle(NavObstacleId id);
    [[nodiscard]] i32 obstacleCount() const;

    // Processes pending obstacle requests; returns true when the navmesh is fully up to date.
    bool update(f32 dt = 0.f);
    // Runs update() until everything is processed (loading screens, tests).
    void flush();

    [[nodiscard]] NavMesh& navMesh() { return *m_navMesh; }
    [[nodiscard]] const NavMesh& navMesh() const { return *m_navMesh; }
    [[nodiscard]] dtTileCache* detour() const { return m_cache; }

    // Obstacle outlines (orange).
    void debugDraw(const DebugLineFn& line) const;

private:
    NavTileCache() = default;
    struct Helpers;
    std::unique_ptr<Helpers> m_helpers;
    dtTileCache* m_cache = nullptr;
    std::unique_ptr<NavMesh> m_navMesh;
};

} // namespace ox::ai
