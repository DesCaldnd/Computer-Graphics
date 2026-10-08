#include "detour_util.hpp"

#include <oxwald/ai/nav_crowd.hpp>
#include <oxwald/core/log.hpp>

#include <DetourCrowd.h>
#include <DetourNavMesh.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ox::ai {

using namespace detail;

namespace {

dtCrowdAgentParams toDetour(const NavAgentParams& p) {
    dtCrowdAgentParams a{};
    a.radius = p.radius;
    a.height = p.height;
    a.maxSpeed = p.maxSpeed;
    a.maxAcceleration = p.maxAcceleration;
    a.collisionQueryRange = p.collisionQueryRange > 0.f ? p.collisionQueryRange : p.radius * 12.f;
    a.pathOptimizationRange = p.pathOptimizationRange > 0.f ? p.pathOptimizationRange : p.radius * 30.f;
    a.separationWeight = p.separationWeight;
    a.updateFlags = 0;
    if (p.anticipateTurns) a.updateFlags |= DT_CROWD_ANTICIPATE_TURNS;
    if (p.optimizeVisibility) a.updateFlags |= DT_CROWD_OPTIMIZE_VIS;
    if (p.optimizeTopology) a.updateFlags |= DT_CROWD_OPTIMIZE_TOPO;
    if (p.separation) a.updateFlags |= DT_CROWD_SEPARATION;
    if (p.obstacleAvoidance) a.updateFlags |= DT_CROWD_OBSTACLE_AVOIDANCE;
    a.obstacleAvoidanceType = static_cast<unsigned char>(std::min<u8>(p.obstacleAvoidanceQuality, 3));
    a.queryFilterType = static_cast<unsigned char>(std::min<u8>(p.filterIndex, DT_CROWD_MAX_QUERY_FILTER_TYPE - 1));
    return a;
}

} // namespace

NavCrowd::NavCrowd(const NavMesh& mesh, i32 maxAgents, f32 maxAgentRadius) : m_mesh(mesh) {
    m_crowd = dtAllocCrowd();
    if (m_crowd == nullptr || mesh.detour() == nullptr || !m_crowd->init(maxAgents, maxAgentRadius, mesh.detour())) {
        OX_LOG_ERROR("ai", "NavCrowd: init failed");
        dtFreeCrowd(m_crowd);
        m_crowd = nullptr;
        return;
    }
    m_targets.resize(static_cast<usize>(maxAgents), glm::vec3(0.f));

    // Obstacle avoidance presets low/medium/good/high (Detour sample values).
    dtObstacleAvoidanceParams params;
    std::memcpy(&params, m_crowd->getObstacleAvoidanceParams(0), sizeof(params));
    params.velBias = 0.5f;
    params.adaptiveDivs = 5;
    params.adaptiveRings = 2;
    params.adaptiveDepth = 1;
    m_crowd->setObstacleAvoidanceParams(0, &params);
    params.adaptiveDivs = 5;
    params.adaptiveRings = 2;
    params.adaptiveDepth = 2;
    m_crowd->setObstacleAvoidanceParams(1, &params);
    params.adaptiveDivs = 7;
    params.adaptiveRings = 2;
    params.adaptiveDepth = 3;
    m_crowd->setObstacleAvoidanceParams(2, &params);
    params.adaptiveDivs = 7;
    params.adaptiveRings = 3;
    params.adaptiveDepth = 3;
    m_crowd->setObstacleAvoidanceParams(3, &params);

    setFilter(0, NavQueryFilter{});
}

NavCrowd::~NavCrowd() { dtFreeCrowd(m_crowd); }

void NavCrowd::setFilter(u8 index, const NavQueryFilter& filter) {
    if (m_crowd != nullptr && index < DT_CROWD_MAX_QUERY_FILTER_TYPE) {
        detail::toDetour(filter, *m_crowd->getEditableFilter(index));
    }
}

NavAgent NavCrowd::addAgent(const glm::vec3& position, const NavAgentParams& params) {
    if (m_crowd == nullptr) {
        return {};
    }
    const dtCrowdAgentParams p = toDetour(params);
    const int idx = m_crowd->addAgent(ptr(position), &p);
    if (idx >= 0) {
        m_targets[static_cast<usize>(idx)] = position;
    }
    return NavAgent{idx};
}

void NavCrowd::removeAgent(NavAgent agent) {
    if (m_crowd != nullptr && agent.valid()) {
        m_crowd->removeAgent(agent.index);
    }
}

void NavCrowd::setParams(NavAgent agent, const NavAgentParams& params) {
    if (m_crowd != nullptr && agent.valid()) {
        const dtCrowdAgentParams p = toDetour(params);
        m_crowd->updateAgentParameters(agent.index, &p);
    }
}

bool NavCrowd::setTarget(NavAgent agent, const glm::vec3& target) {
    if (m_crowd == nullptr || !agent.valid()) {
        return false;
    }
    const dtCrowdAgent* a = m_crowd->getAgent(agent.index);
    if (a == nullptr || !a->active) {
        return false;
    }
    const dtQueryFilter* filter = m_crowd->getFilter(a->params.queryFilterType);
    dtPolyRef ref = 0;
    glm::vec3 nearest{};
    m_crowd->getNavMeshQuery()->findNearestPoly(ptr(target), m_crowd->getQueryExtents(), filter, &ref, &nearest.x);
    if (ref == 0) {
        return false;
    }
    m_targets[static_cast<usize>(agent.index)] = nearest;
    return m_crowd->requestMoveTarget(agent.index, ref, ptr(nearest));
}

void NavCrowd::setVelocity(NavAgent agent, const glm::vec3& velocity) {
    if (m_crowd != nullptr && agent.valid()) {
        m_crowd->requestMoveVelocity(agent.index, ptr(velocity));
    }
}

void NavCrowd::resetTarget(NavAgent agent) {
    if (m_crowd != nullptr && agent.valid()) {
        m_crowd->resetMoveTarget(agent.index);
    }
}

void NavCrowd::update(f32 dt) {
    if (m_crowd != nullptr && dt > 0.f) {
        m_crowd->update(dt, nullptr);
    }
}

glm::vec3 NavCrowd::position(NavAgent agent) const {
    const dtCrowdAgent* a = (m_crowd != nullptr && agent.valid()) ? m_crowd->getAgent(agent.index) : nullptr;
    return a != nullptr ? vec(a->npos) : glm::vec3(0.f);
}

glm::vec3 NavCrowd::velocity(NavAgent agent) const {
    const dtCrowdAgent* a = (m_crowd != nullptr && agent.valid()) ? m_crowd->getAgent(agent.index) : nullptr;
    return a != nullptr ? vec(a->vel) : glm::vec3(0.f);
}

glm::vec3 NavCrowd::target(NavAgent agent) const {
    return (agent.valid() && static_cast<usize>(agent.index) < m_targets.size()) ? m_targets[static_cast<usize>(agent.index)]
                                                                                 : glm::vec3(0.f);
}

NavAgentState NavCrowd::state(NavAgent agent) const {
    const dtCrowdAgent* a = (m_crowd != nullptr && agent.valid()) ? m_crowd->getAgent(agent.index) : nullptr;
    if (a == nullptr || !a->active) {
        return NavAgentState::Invalid;
    }
    switch (a->state) {
    case DT_CROWDAGENT_STATE_WALKING: return NavAgentState::Walking;
    case DT_CROWDAGENT_STATE_OFFMESH: return NavAgentState::OffMeshLink;
    default: return NavAgentState::Invalid;
    }
}

bool NavCrowd::reachedTarget(NavAgent agent, f32 tolerance) const {
    const glm::vec3 p = position(agent), t = target(agent);
    const glm::vec2 d(p.x - t.x, p.z - t.z);
    return glm::length(d) <= tolerance && std::fabs(p.y - t.y) < 1.f;
}

i32 NavCrowd::agentCount() const {
    if (m_crowd == nullptr) {
        return 0;
    }
    i32 n = 0;
    for (int i = 0; i < m_crowd->getAgentCount(); ++i) {
        const dtCrowdAgent* a = m_crowd->getAgent(i);
        n += (a != nullptr && a->active) ? 1 : 0;
    }
    return n;
}

void NavCrowd::debugDraw(const DebugLineFn& line) const {
    if (m_crowd == nullptr || !line) {
        return;
    }
    for (int i = 0; i < m_crowd->getAgentCount(); ++i) {
        const dtCrowdAgent* a = m_crowd->getAgent(i);
        if (a == nullptr || !a->active) {
            continue;
        }
        const glm::vec3 p = vec(a->npos);
        constexpr int kSeg = 16;
        for (int s = 0; s < kSeg; ++s) {
            const f32 a0 = 6.2831853f * static_cast<f32>(s) / kSeg, a1 = 6.2831853f * static_cast<f32>(s + 1) / kSeg;
            line(p + glm::vec3(std::cos(a0), 0.05f, std::sin(a0)) * a->params.radius,
                 p + glm::vec3(std::cos(a1), 0.05f, std::sin(a1)) * a->params.radius, {0.f, 1.f, 0.f, 1.f});
        }
        line(p, p + glm::vec3(0, a->params.height, 0), {0.f, 0.6f, 0.f, 1.f});
        line(p + glm::vec3(0, 0.1f, 0), p + glm::vec3(0, 0.1f, 0) + vec(a->vel), {1.f, 1.f, 0.f, 1.f});
        if (a->ncorners > 0) {
            line(p, vec(&a->cornerVerts[0]), {1.f, 1.f, 1.f, 0.5f});
        }
    }
}

} // namespace ox::ai
