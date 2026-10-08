#pragma once

#include <oxwald/ai/navmesh.hpp>

#include <memory>
#include <optional>
#include <vector>

class dtNavMeshQuery;

namespace ox::ai {

enum class PathStatus : u8 {
    Failed,   // start or end not on the navmesh
    Partial,  // target unreachable: path leads to the closest reachable point
    Complete,
};

enum class PathPointFlag : u8 { None = 0, Start = 1, End = 2, OffMeshLink = 4 };

struct NavPath {
    PathStatus status = PathStatus::Failed;
    std::vector<NavPolyRef> corridor; // polygon corridor start → end
    std::vector<glm::vec3> points;    // string-pulled straight path
    std::vector<u8> pointFlags;       // PathPointFlag bits per point
    [[nodiscard]] bool found() const { return status != PathStatus::Failed && !points.empty(); }
    [[nodiscard]] f32 length() const;
};

struct NavPoint {
    glm::vec3 position{0.f};
    NavPolyRef poly = 0;
};

struct NavRaycastHit {
    bool hit = false;        // true when a wall was hit before reaching the end
    f32 t = 1.f;             // fraction of the segment travelled
    glm::vec3 position{0.f}; // hit position (or end)
    glm::vec3 normal{0.f};
    std::vector<NavPolyRef> visited;
};

// Wraps dtNavMeshQuery. Not thread-safe: use one query object per thread/agent system.
class NavQuery {
public:
    explicit NavQuery(const NavMesh& mesh, i32 maxNodes = 4096);
    ~NavQuery();
    NavQuery(const NavQuery&) = delete;
    NavQuery& operator=(const NavQuery&) = delete;

    [[nodiscard]] bool valid() const { return m_query != nullptr; }

    // Search box half-extents used to snap points to the mesh.
    void setSearchExtents(const glm::vec3& halfExtents) { m_extents = halfExtents; }

    [[nodiscard]] NavPath findPath(const glm::vec3& start, const glm::vec3& end, const NavQueryFilter& filter = {},
                                   i32 maxPolys = 512) const;
    [[nodiscard]] std::optional<NavPoint> nearestPoint(const glm::vec3& pos, const NavQueryFilter& filter = {}) const;
    [[nodiscard]] NavRaycastHit raycast(const glm::vec3& start, const glm::vec3& end, const NavQueryFilter& filter = {}) const;
    // Uniform over area. `seed` makes the result deterministic.
    [[nodiscard]] std::optional<NavPoint> randomPoint(u32 seed, const NavQueryFilter& filter = {}) const;
    [[nodiscard]] std::optional<NavPoint> randomPointInRadius(const glm::vec3& center, f32 radius, u32 seed,
                                                              const NavQueryFilter& filter = {}) const;
    // Densely sampled path following the surface (Detour sample "smooth path"); handles off-mesh links.
    [[nodiscard]] std::vector<glm::vec3> smoothPath(const glm::vec3& start, const glm::vec3& end,
                                                    const NavQueryFilter& filter = {}, f32 stepSize = 0.5f,
                                                    i32 maxPoints = 2048) const;
    [[nodiscard]] std::optional<f32> heightAt(const glm::vec3& pos, const NavQueryFilter& filter = {}) const;

    [[nodiscard]] dtNavMeshQuery* detour() const { return m_query; }
    [[nodiscard]] const NavMesh& mesh() const { return m_mesh; }

private:
    const NavMesh& m_mesh;
    dtNavMeshQuery* m_query = nullptr;
    glm::vec3 m_extents{2.f, 4.f, 2.f};
};

} // namespace ox::ai
