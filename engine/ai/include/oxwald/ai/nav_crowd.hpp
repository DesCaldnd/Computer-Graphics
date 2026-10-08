#pragma once

#include <oxwald/ai/navmesh.hpp>

#include <compare>
#include <memory>

class dtCrowd;

namespace ox::ai {

struct NavAgent {
    i32 index = -1;
    [[nodiscard]] bool valid() const { return index >= 0; }
    explicit operator bool() const { return valid(); }
    auto operator<=>(const NavAgent&) const = default;
};

struct NavAgentParams {
    f32 radius = 0.5f;
    f32 height = 2.f;
    f32 maxSpeed = 3.5f;
    f32 maxAcceleration = 8.f;
    f32 collisionQueryRange = 0.f;   // 0 → radius * 12
    f32 pathOptimizationRange = 0.f; // 0 → radius * 30
    f32 separationWeight = 2.f;
    u8 obstacleAvoidanceQuality = 3; // 0 low .. 3 high
    bool anticipateTurns = true;
    bool optimizeVisibility = true;
    bool optimizeTopology = true;
    bool separation = true;
    bool obstacleAvoidance = true;
    u8 filterIndex = 0; // index into NavCrowd::setFilter
};

enum class NavAgentState : u8 { Invalid, Walking, OffMeshLink };

// DetourCrowd wrapper: local steering, separation and obstacle avoidance for many agents.
class NavCrowd {
public:
    NavCrowd(const NavMesh& mesh, i32 maxAgents = 128, f32 maxAgentRadius = 1.f);
    ~NavCrowd();
    NavCrowd(const NavCrowd&) = delete;
    NavCrowd& operator=(const NavCrowd&) = delete;

    [[nodiscard]] bool valid() const { return m_crowd != nullptr; }

    NavAgent addAgent(const glm::vec3& position, const NavAgentParams& params = {});
    void removeAgent(NavAgent agent);
    void setParams(NavAgent agent, const NavAgentParams& params);
    // Snaps the target to the navmesh; false when no polygon is near.
    bool setTarget(NavAgent agent, const glm::vec3& target);
    void setVelocity(NavAgent agent, const glm::vec3& velocity); // direct steering, no path
    void resetTarget(NavAgent agent);
    void setFilter(u8 index, const NavQueryFilter& filter);

    void update(f32 dt);

    [[nodiscard]] glm::vec3 position(NavAgent agent) const;
    [[nodiscard]] glm::vec3 velocity(NavAgent agent) const;
    [[nodiscard]] glm::vec3 target(NavAgent agent) const;
    [[nodiscard]] NavAgentState state(NavAgent agent) const;
    [[nodiscard]] bool reachedTarget(NavAgent agent, f32 tolerance = 0.3f) const;
    [[nodiscard]] i32 agentCount() const;

    // Agent cylinders (green), velocities (yellow), corridor targets (white).
    void debugDraw(const DebugLineFn& line) const;

    [[nodiscard]] dtCrowd* detour() const { return m_crowd; }
    [[nodiscard]] const NavMesh& mesh() const { return m_mesh; }

private:
    const NavMesh& m_mesh;
    dtCrowd* m_crowd = nullptr;
    std::vector<glm::vec3> m_targets;
};

} // namespace ox::ai
