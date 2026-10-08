#include "world_runtime_impl.hpp"

#include <oxwald/core/profile.hpp>
#include <oxwald/gameplay/common.hpp>
#include <oxwald/world/physics_bridge.hpp>

#include <algorithm>
#include <cmath>

namespace ox::gameplay {

namespace {

f32 distanceToRect(glm::vec2 p, glm::vec2 lo, glm::vec2 hi) {
    const glm::vec2 d = glm::max(glm::max(lo - p, p - hi), glm::vec2(0.f));
    return glm::length(d);
}

} // namespace

void WorldRuntime::Impl::clearVegetation(VegetationState& st) {
    for (auto& [c, chunk] : st.chunks) {
        for (auto& b : chunk.bodies) destroyBody(b);
    }
    st.chunks.clear();
}

void WorldRuntime::Impl::buildVegetationBodies(Entity e, VegetationChunkState& chunk) {
    chunk.bodiesBuilt = true;
    const u64 id = toRuntimeId(e);
    // Collider pose is already the body pose (lifted to stand on the ground, yaw only).
    for (const world::VegetationCollider& c : chunk.chunk.colliders) {
        physics::BodyHandle b = createStaticBody(world::toShapeDesc(c), c.position, c.rotation, id, 0.6f, 0.f, true);
        if (b) chunk.bodies.push_back(b);
    }
}

void WorldRuntime::Impl::updateVegetation(Entity e, VegetationState& st, const std::vector<glm::vec3>& viewerPositions) {
    const auto& c = e.get<VegetationComponent>();
    Entity terrainEnt = c.terrain.valid() ? world->resolve(c.terrain) : e;
    auto tit = terrainEnt.valid() ? terrains.find(terrainEnt.handle()) : terrains.end();
    const TerrainState* terrain = tit != terrains.end() && tit->second.hf ? &tit->second : nullptr;
    if (!terrain) {
        if (!st.chunks.empty()) clearVegetation(st);
        st.terrain = entt::null;
        return;
    }
    if (st.dirty || st.terrain != terrainEnt.handle() || st.terrainVersion != terrain->version || !st.scatterer) {
        clearVegetation(st);
        st.scatterer = std::make_unique<world::VegetationScatterer>(c.layers, std::max(8.f, c.patternPeriod));
        st.terrain = terrainEnt.handle();
        st.terrainVersion = terrain->version;
        st.dirty = false;
    }
    const world::HeightfieldDesc& d = terrain->hf->desc();
    const glm::vec2 lo = d.origin, hi = d.origin + glm::vec2(d.worldSize);
    const f32 chunkSize = std::max(1.f, c.chunkSize);
    const f32 radius = std::max(0.f, c.scatterRadius);

    std::vector<glm::vec2> viewers;
    for (const glm::vec3& p : viewerPositions) viewers.emplace_back(p.x, p.z);
    if (viewers.empty()) viewers.push_back((lo + hi) * 0.5f);

    // Drop chunks far from every viewer (hysteresis 1.25x).
    for (auto it = st.chunks.begin(); it != st.chunks.end();) {
        const glm::vec2 clo = glm::vec2(f32(it->first.first), f32(it->first.second)) * chunkSize;
        f32 best = 1e30f;
        for (const glm::vec2 v : viewers) best = std::min(best, distanceToRect(v, clo, clo + glm::vec2(chunkSize)));
        if (best > radius * 1.25f) {
            for (auto& b : it->second.bodies) destroyBody(b);
            it = st.chunks.erase(it);
        } else {
            ++it;
        }
    }

    // Wanted chunks, nearest first.
    std::vector<std::pair<f32, std::pair<i32, i32>>> wanted;
    for (const glm::vec2 v : viewers) {
        const glm::vec2 a = glm::max(v - glm::vec2(radius), lo), b = glm::min(v + glm::vec2(radius), hi);
        if (a.x >= b.x || a.y >= b.y) continue;
        const i32 x0 = i32(std::floor(a.x / chunkSize)), x1 = i32(std::floor((b.x - 1e-4f) / chunkSize));
        const i32 z0 = i32(std::floor(a.y / chunkSize)), z1 = i32(std::floor((b.y - 1e-4f) / chunkSize));
        for (i32 z = z0; z <= z1; ++z) {
            for (i32 x = x0; x <= x1; ++x) {
                if (st.chunks.contains({x, z})) continue;
                const glm::vec2 clo = glm::vec2(f32(x), f32(z)) * chunkSize;
                const f32 dist = distanceToRect(v, clo, clo + glm::vec2(chunkSize));
                if (dist <= radius) wanted.push_back({dist, {x, z}});
            }
        }
    }
    std::sort(wanted.begin(), wanted.end());
    wanted.erase(std::unique(wanted.begin(), wanted.end(), [](const auto& a, const auto& b) { return a.second == b.second; }),
                 wanted.end());

    world::ScatterContext ctx;
    ctx.heightfield = terrain->hf.get();
    ctx.splat = terrain->splat.get();
    ctx.exclusions = c.exclusions;
    u32 budget = std::max(1u, config.maxVegetationChunksPerFrame);
    for (const auto& [dist, coord] : wanted) {
        if (st.chunks.contains(coord)) continue;
        if (budget-- == 0) break;
        // Points outside the terrain are rejected by the scatterer.
        const glm::vec2 clo = glm::vec2(f32(coord.first), f32(coord.second)) * chunkSize;
        VegetationChunkState cs;
        cs.chunk = st.scatterer->scatter(clo, chunkSize, ctx, std::max(1.f, c.cellSize));
        const auto& layers = st.scatterer->layers();
        auto gpu = std::make_shared<std::vector<world::VegetationInstanceGpu>>();
        gpu->reserve(cs.chunk.instances.size());
        for (const auto& inst : cs.chunk.instances) {
            gpu->push_back(world::toGpu(inst, inst.layer < layers.size() ? layers[inst.layer] : world::VegetationLayer{}));
        }
        cs.gpu = std::move(gpu);
        cs.cells = std::make_shared<const std::vector<world::VegetationCell>>(cs.chunk.cells);
        cs.version = ++vegetationCounter;
        st.chunks.emplace(coord, std::move(cs));
    }

    // Tree colliders (play mode only).
    if (physicsEnabled() && c.colliders) {
        bool any = false;
        for (auto& [coord, chunk] : st.chunks) {
            if (chunk.bodiesBuilt) continue;
            buildVegetationBodies(e, chunk);
            any = any || !chunk.bodies.empty();
        }
        if (any) physicsRt->physicsWorld().optimizeBroadPhase();
    }

    auto& mc = world->registry().get<VegetationComponent>(e.handle());
    mc.chunkCount = static_cast<u32>(st.chunks.size());
    usize n = 0;
    for (const auto& [coord, chunk] : st.chunks) n += chunk.chunk.instances.size();
    mc.instanceCount = static_cast<u32>(n);
}

void WorldRuntime::updateVegetation() {
    OX_PROFILE_ZONE();
    if (!m->world || !m->config.vegetation) return;
    entt::registry& r = m->world->registry();
    std::vector<entt::entity> list;
    for (auto e : r.view<VegetationComponent>()) list.push_back(e);
    if (list.empty()) return;
    std::sort(list.begin(), list.end());
    std::vector<glm::vec3> positions;
    for (const world::StreamingViewer& v : m->viewers(true)) positions.push_back(v.position);
    for (auto e : list) m->updateVegetation(m->world->wrap(e), m->vegetation[e], positions);
}

} // namespace ox::gameplay
