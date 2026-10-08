#include <oxwald/core/debug_draw.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/core/profile.hpp>
#include <oxwald/gameplay/ai.hpp>
#include <oxwald/gameplay/animation.hpp>
#include <oxwald/gameplay/audio.hpp>
#include <oxwald/gameplay/physics.hpp>
#include <oxwald/gameplay/providers.hpp>
#include <oxwald/scene/components.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/norm.hpp>

namespace ox::gameplay {

namespace {

using nlohmann::json;

// Same face order/winding as ai::NavMeshInput::addBox (CCW seen from outside), corners transformed by m.
void addTransformedBox(ai::NavMeshInput& in, const glm::mat4& m, const glm::vec3& c, const glm::vec3& h) {
    const glm::vec3 mn = c - h, mx = c + h;
    glm::vec3 p[8] = {{mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, {mx.x, mn.y, mx.z}, {mn.x, mn.y, mx.z},
                      {mn.x, mx.y, mn.z}, {mx.x, mx.y, mn.z}, {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z}};
    for (auto& v : p) v = glm::vec3(m * glm::vec4(v, 1.f));
    in.addQuad(p[4], p[7], p[6], p[5]);
    in.addQuad(p[0], p[1], p[2], p[3]);
    in.addQuad(p[0], p[4], p[5], p[1]);
    in.addQuad(p[2], p[6], p[7], p[3]);
    in.addQuad(p[3], p[7], p[4], p[0]);
    in.addQuad(p[1], p[5], p[6], p[2]);
}

void addTriangles(ai::NavMeshInput& in, const glm::mat4& m, const std::vector<glm::vec3>& verts,
                  const std::vector<u32>& indices) {
    const u32 base = static_cast<u32>(in.vertices.size());
    in.triAreas.resize(in.indices.size() / 3, ai::NavArea::Ground);
    for (const auto& v : verts) in.vertices.push_back(glm::vec3(m * glm::vec4(v, 1.f)));
    for (u32 i : indices) in.indices.push_back(base + i);
    in.triAreas.resize(in.indices.size() / 3, ai::NavArea::Ground);
}

glm::vec3 primitiveHalfExtents(ColliderType type, const glm::vec3& he, f32 r, f32 hh) {
    switch (type) {
    case ColliderType::Sphere: return glm::vec3(r);
    case ColliderType::Capsule: return {r, hh + r, r};
    case ColliderType::Cylinder: return {r, hh, r};
    default: return he;
    }
}

void addCollider(ai::NavMeshInput& in, Entity e, const ColliderComponent& c, IMeshColliderProvider* meshes) {
    const Transform wt = e.worldTransform();
    const glm::vec3 s = c.scale * wt.scale;
    const glm::mat4 base = glm::translate(glm::mat4(1.f), wt.position) * glm::mat4_cast(glm::normalize(wt.rotation)) *
                           glm::translate(glm::mat4(1.f), c.offsetPosition * s) *
                           glm::mat4_cast(glm::normalize(c.offsetRotation));
    const glm::mat4 scaled = base * glm::scale(glm::mat4(1.f), s);
    switch (c.type) {
    case ColliderType::Box:
    case ColliderType::Sphere:
    case ColliderType::Capsule:
    case ColliderType::Cylinder:
        addTransformedBox(in, scaled, glm::vec3(0.f), primitiveHalfExtents(c.type, c.halfExtents, c.radius, c.halfHeight));
        break;
    case ColliderType::ConvexHull:
    case ColliderType::Mesh: {
        std::shared_ptr<const MeshTriangles> mesh = meshes && c.mesh.isValid() ? meshes->meshTriangles(c.mesh) : nullptr;
        if (c.type == ColliderType::Mesh && mesh) {
            addTriangles(in, scaled, mesh->vertices, mesh->indices);
        } else {
            const auto& pts = !c.points.empty() || !mesh ? c.points : mesh->vertices;
            if (pts.empty()) break;
            glm::vec3 mn(1e30f), mx(-1e30f);
            for (const auto& p : pts) {
                mn = glm::min(mn, p);
                mx = glm::max(mx, p);
            }
            addTransformedBox(in, scaled, (mn + mx) * 0.5f, (mx - mn) * 0.5f);
        }
        break;
    }
    case ColliderType::HeightField: {
        const u32 n = c.sampleCount;
        if (n < 2 || c.heights.size() != usize(n) * n) break;
        std::vector<glm::vec3> verts;
        verts.reserve(usize(n) * n);
        for (u32 z = 0; z < n; ++z) {
            for (u32 x = 0; x < n; ++x) {
                const f32 h = c.heights[usize(z) * n + x];
                verts.push_back(c.heightFieldOffset + c.heightFieldScale * glm::vec3(f32(x), h >= 1e30f ? 0.f : h, f32(z)));
            }
        }
        std::vector<u32> idx;
        for (u32 z = 0; z + 1 < n; ++z) {
            for (u32 x = 0; x + 1 < n; ++x) {
                const u32 v00 = z * n + x, v10 = v00 + 1, v01 = v00 + n, v11 = v01 + 1;
                if (c.heights[v00] >= 1e30f || c.heights[v10] >= 1e30f || c.heights[v01] >= 1e30f ||
                    c.heights[v11] >= 1e30f) {
                    continue; // hole
                }
                idx.insert(idx.end(), {v00, v01, v10, v10, v01, v11});
            }
        }
        addTriangles(in, scaled, verts, idx);
        break;
    }
    case ColliderType::Compound:
        for (const auto& ch : c.children) {
            const glm::mat4 cm = base * glm::scale(glm::mat4(1.f), s) * glm::translate(glm::mat4(1.f), ch.position) *
                                 glm::mat4_cast(glm::normalize(ch.rotation));
            addTransformedBox(in, cm, glm::vec3(0.f), primitiveHalfExtents(ch.type, ch.halfExtents, ch.radius, ch.halfHeight));
        }
        break;
    }
}

// ---- gameplay behaviour tree nodes -----------------------------------------------------------------------

BTAgentContext* agentContext(ai::BTContext& ctx) { return static_cast<BTAgentContext*>(ctx.user); }

std::optional<glm::vec3> blackboardPosition(ai::BTContext& ctx, const std::string& key) {
    const ai::BlackboardValue* v = ctx.blackboard.find(key);
    if (!v) return std::nullopt;
    if (const auto* p = std::get_if<glm::vec3>(v)) return *p;
    if (const auto* id = std::get_if<u64>(v)) {
        BTAgentContext* a = agentContext(ctx);
        if (!a || !a->world) return std::nullopt;
        const Entity e = entityFromRuntimeId(*a->world, *id);
        if (e.valid()) return e.worldPosition();
    }
    return std::nullopt;
}

class BTMoveTo final : public ai::BTNode {
public:
    BTMoveTo(std::optional<glm::vec3> target, std::string key, f32 acceptance, std::string name)
        : BTNode(std::move(name)), m_target(target), m_key(std::move(key)), m_acceptance(acceptance) {}
    const char* typeName() const override { return "MoveTo"; }
    void saveParams(json& j) const override {
        if (m_target) j["target"] = {m_target->x, m_target->y, m_target->z};
        if (!m_key.empty()) j["key"] = m_key;
        j["acceptance"] = m_acceptance;
    }

protected:
    void onEnter(ai::BTContext& ctx) override {
        m_failed = true;
        BTAgentContext* a = agentContext(ctx);
        if (!a || !a->ai) return;
        auto dest = resolve(ctx);
        if (!dest) return;
        m_dest = *dest;
        m_failed = !a->ai->moveTo(a->self(), m_dest);
    }
    ai::BTStatus onTick(ai::BTContext& ctx) override {
        BTAgentContext* a = agentContext(ctx);
        if (m_failed || !a) return ai::BTStatus::Failure;
        const Entity self = a->self();
        const auto* agent = self.tryGet<NavAgentComponent>();
        if (!agent) return ai::BTStatus::Failure;
        if (!m_key.empty()) {
            // Moving targets (entities): re-path when they moved noticeably.
            if (auto dest = resolve(ctx); dest && glm::distance2(*dest, m_dest) > 1.f) {
                m_dest = *dest;
                a->ai->moveTo(self, m_dest);
            }
        }
        glm::vec3 d = self.worldPosition() - m_dest;
        d.y = 0.f;
        if (glm::length(d) <= std::max(m_acceptance, agent->stoppingDistance)) return ai::BTStatus::Success;
        return ai::BTStatus::Running;
    }
    void onAbort(ai::BTContext& ctx) override {
        if (BTAgentContext* a = agentContext(ctx); a && a->ai) a->ai->stop(a->self());
    }

private:
    std::optional<glm::vec3> resolve(ai::BTContext& ctx) const {
        if (!m_key.empty()) return blackboardPosition(ctx, m_key);
        return m_target;
    }
    std::optional<glm::vec3> m_target;
    std::string m_key;
    f32 m_acceptance;
    glm::vec3 m_dest{0.f};
    bool m_failed = false;
};

class BTPlayAnimation final : public ai::BTNode {
public:
    BTPlayAnimation(std::string trigger, std::string state, std::string name)
        : BTNode(std::move(name)), m_trigger(std::move(trigger)), m_state(std::move(state)) {}
    const char* typeName() const override { return "PlayAnimation"; }
    void saveParams(json& j) const override {
        if (!m_trigger.empty()) j["trigger"] = m_trigger;
        if (!m_state.empty()) j["state"] = m_state;
    }

protected:
    ai::BTStatus onTick(ai::BTContext& ctx) override {
        BTAgentContext* a = agentContext(ctx);
        AnimationRuntime* anim = a && a->ai ? a->ai->animation() : nullptr;
        if (!anim) return ai::BTStatus::Failure;
        const Entity self = a->self();
        if (!m_trigger.empty()) {
            if (!anim->animator(self)) return ai::BTStatus::Failure;
            anim->setTrigger(self, m_trigger);
            return ai::BTStatus::Success;
        }
        return anim->play(self, m_state, 0, 0.2f) ? ai::BTStatus::Success : ai::BTStatus::Failure;
    }

private:
    std::string m_trigger;
    std::string m_state;
};

class BTPlaySound final : public ai::BTNode {
public:
    BTPlaySound(std::string clip, f32 volume, bool spatial, std::string name)
        : BTNode(std::move(name)), m_clip(std::move(clip)), m_volume(volume), m_spatial(spatial) {}
    const char* typeName() const override { return "PlaySound"; }
    void saveParams(json& j) const override {
        j["clip"] = m_clip;
        j["volume"] = m_volume;
        j["spatial"] = m_spatial;
    }

protected:
    ai::BTStatus onTick(ai::BTContext& ctx) override {
        BTAgentContext* a = agentContext(ctx);
        AudioRuntime* audio = a && a->ai ? a->ai->audio() : nullptr;
        if (!audio) return ai::BTStatus::Failure;
        const glm::vec3 pos = a->self().worldPosition();
        return audio->playOneShot(m_clip, m_spatial ? &pos : nullptr, m_volume) ? ai::BTStatus::Success
                                                                                 : ai::BTStatus::Failure;
    }

private:
    std::string m_clip;
    f32 m_volume;
    bool m_spatial;
};

class BTIsTargetVisible final : public ai::BTNode {
public:
    BTIsTargetVisible(std::string key, std::string name) : BTNode(std::move(name)), m_key(std::move(key)) {}
    const char* typeName() const override { return "IsTargetVisible"; }
    void saveParams(json& j) const override { j["key"] = m_key; }

protected:
    ai::BTStatus onTick(ai::BTContext& ctx) override {
        BTAgentContext* a = agentContext(ctx);
        if (!a || !a->ai) return ai::BTStatus::Failure;
        const Entity self = a->self();
        const auto* p = self.tryGet<PerceptionComponent>();
        if (!p) return ai::BTStatus::Failure;
        if (auto id = ctx.blackboard.get<u64>(m_key)) {
            const Entity target = entityFromRuntimeId(*a->world, *id);
            return target.valid() && p->target.valid() && p->target == target.ref() && p->targetVisible
                       ? ai::BTStatus::Success
                       : ai::BTStatus::Failure;
        }
        if (!p->targetVisible) return ai::BTStatus::Failure;
        const Entity target = a->world->resolve(p->target);
        if (!target.valid()) return ai::BTStatus::Failure;
        ctx.blackboard.set(m_key, toBlackboardId(target));
        ctx.blackboard.set(m_key + "Position", target.worldPosition());
        return ai::BTStatus::Success;
    }

private:
    std::string m_key;
};

class BTScriptAction final : public ai::BTNode {
public:
    BTScriptAction(std::string function, std::string name) : BTNode(std::move(name)), m_function(std::move(function)) {}
    const char* typeName() const override { return "ScriptAction"; }
    void saveParams(json& j) const override { j["function"] = m_function; }

protected:
    ai::BTStatus onTick(ai::BTContext& ctx) override {
        BTAgentContext* a = agentContext(ctx);
        if (!a || !a->ai || !a->ai->scriptActionHook) return ai::BTStatus::Failure;
        return a->ai->scriptActionHook(a->self(), m_function);
    }

private:
    std::string m_function;
};

std::string nodeName(const json& j, const char* fallback) {
    auto it = j.find("name");
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string(fallback);
}

} // namespace

ai::NavMeshBuildSettings toBuildSettings(const NavMeshSurfaceComponent& c) {
    ai::NavMeshBuildSettings s;
    s.cellSize = c.cellSize;
    s.cellHeight = c.cellHeight;
    s.agentHeight = c.agentHeight;
    s.agentRadius = c.agentRadius;
    s.agentMaxClimb = c.agentMaxClimb;
    s.agentMaxSlope = c.agentMaxSlope;
    s.regionMinSize = c.regionMinSize;
    s.regionMergeSize = c.regionMergeSize;
    s.edgeMaxLen = c.edgeMaxLen;
    s.edgeMaxError = c.edgeMaxError;
    s.vertsPerPoly = c.vertsPerPoly;
    s.detailSampleDist = c.detailSampleDist;
    s.detailSampleMaxError = c.detailSampleMaxError;
    s.tiled = c.tiled;
    s.tileSize = c.tileSize;
    return s;
}

ai::NavMeshInput gatherNavMeshInput(World& world, const NavMeshSurfaceComponent& s, Entity surface,
                                    IMeshColliderProvider* meshes) {
    OX_PROFILE_ZONE();
    ai::NavMeshInput in;
    auto inScope = [&](Entity e) {
        return !s.onlyChildren || !surface.valid() || e == surface || surface.isAncestorOf(e);
    };
    auto isStatic = [](Entity e) {
        const auto* rb = e.tryGet<RigidBodyComponent>();
        return !rb || rb->motionType == physics::MotionType::Static;
    };
    entt::registry& r = world.registry();
    if (s.includeStaticColliders) {
        for (auto [handle, c] : r.view<ColliderComponent>().each()) {
            const Entity e = world.wrap(handle);
            if (c.isSensor || e.has<TriggerComponent>() || !isStatic(e) || !inScope(e) || !e.activeInHierarchy()) continue;
            addCollider(in, e, c, meshes);
        }
    }
    if (s.includeMeshes && meshes) {
        for (auto [handle, mr] : r.view<MeshRendererComponent>().each()) {
            const Entity e = world.wrap(handle);
            if (!mr.mesh.isValid() || !isStatic(e) || !inScope(e) || !e.activeInHierarchy()) continue;
            if (auto mesh = meshes->meshTriangles(mr.mesh)) addTriangles(in, e.worldMatrix(), mesh->vertices, mesh->indices);
        }
    }
    return in;
}

// ---- runtime -------------------------------------------------------------------------------------------------

AIRuntime::AIRuntime() { registerGameplayNodes(); }
AIRuntime::~AIRuntime() { detach(); }

void AIRuntime::registerGameplayNodes() {
    m_factory.registerNode("MoveTo", [](const json& j, const ai::BTFactory&) -> std::unique_ptr<ai::BTNode> {
        std::optional<glm::vec3> target;
        if (auto it = j.find("target"); it != j.end() && it->is_array() && it->size() == 3) {
            target = glm::vec3((*it)[0].get<f32>(), (*it)[1].get<f32>(), (*it)[2].get<f32>());
        }
        return std::make_unique<BTMoveTo>(target, j.value("key", std::string()), j.value("acceptance", 0.5f),
                                          nodeName(j, "MoveTo"));
    });
    m_factory.registerNode("PlayAnimation", [](const json& j, const ai::BTFactory&) -> std::unique_ptr<ai::BTNode> {
        return std::make_unique<BTPlayAnimation>(j.value("trigger", std::string()), j.value("state", std::string()),
                                                 nodeName(j, "PlayAnimation"));
    });
    m_factory.registerNode("PlaySound", [](const json& j, const ai::BTFactory&) -> std::unique_ptr<ai::BTNode> {
        return std::make_unique<BTPlaySound>(j.value("clip", std::string()), j.value("volume", 1.f),
                                             j.value("spatial", true), nodeName(j, "PlaySound"));
    });
    m_factory.registerNode("IsTargetVisible", [](const json& j, const ai::BTFactory&) -> std::unique_ptr<ai::BTNode> {
        return std::make_unique<BTIsTargetVisible>(j.value("key", std::string("target")), nodeName(j, "IsTargetVisible"));
    });
    m_factory.registerNode("ScriptAction", [](const json& j, const ai::BTFactory&) -> std::unique_ptr<ai::BTNode> {
        return std::make_unique<BTScriptAction>(j.value("function", std::string()), nodeName(j, "ScriptAction"));
    });
}

void AIRuntime::attach(World& world, Services& services) {
    detach();
    m_world = &world;
    m_services = &services;
    m_physics = services.tryGet<PhysicsRuntime>();
    m_animation = services.tryGet<AnimationRuntime>();
    m_audio = services.tryGet<AudioRuntime>();
    m_bus = services.tryGet<EventBus>();
    entt::registry& r = world.registry();
    m_connections.emplace_back(r.on_destroy<NavAgentComponent>().connect<&AIRuntime::onAgentDestroyed>(*this));
    m_connections.emplace_back(r.on_construct<BehaviorTreeComponent>().connect<&AIRuntime::onTreeChanged>(*this));
    m_connections.emplace_back(r.on_update<BehaviorTreeComponent>().connect<&AIRuntime::onTreeChanged>(*this));
    m_connections.emplace_back(r.on_destroy<BehaviorTreeComponent>().connect<&AIRuntime::onTreeDestroyed>(*this));
    m_connections.emplace_back(r.on_destroy<PerceptionComponent>().connect<&AIRuntime::onPerceptionDestroyed>(*this));
    m_connections.emplace_back(r.on_destroy<NavObstacleComponent>().connect<&AIRuntime::onObstacleDestroyed>(*this));

    m_perception.setRaycast([this](const glm::vec3& a, const glm::vec3& b) {
        if (!m_physics) return false;
        // Agents/targets (entities with perception) don't block their own line of sight.
        return m_physics->countHits(a, b, [](Entity e) { return e.has<PerceptionComponent>(); }) > 0;
    });
    m_perception.setEventCallback([this](ai::PerceptionListenerId id, const ai::PerceivedStimulus& s, bool gained) {
        PerceptionEvent ev;
        for (auto& [e, lid] : m_listeners) {
            if (lid == id) ev.listener = m_world->wrap(e);
        }
        ev.source = entityFromRuntimeId(*m_world, s.sourceId);
        ev.gained = gained;
        ev.sight = s.sense == ai::Sense::Sight;
        m_perceptionEvents.push_back(ev);
    });
}

void AIRuntime::detach() {
    if (!m_world) return;
    syncPlayState(false);
    resetNavigation();
    m_connections.clear();
    m_perception.setRaycast({});
    m_perception.setEventCallback({});
    m_world = nullptr;
}

void AIRuntime::resetNavigation() {
    m_agents.clear();
    m_obstacles.clear();
    m_crowd.reset();
    m_query.reset();
    m_tileCache.reset();
    m_mesh.reset();
    m_surfaceChecked = false;
}

void AIRuntime::syncPlayState(bool playing) {
    if (playing == m_playing) return;
    m_playing = playing;
    m_trees.clear();
    for (auto& [e, id] : m_listeners) {
        m_perception.removeListener(id);
        m_perception.removeSource(toRuntimeId(e));
    }
    m_listeners.clear();
    resetNavigation();
}

void AIRuntime::onAgentDestroyed(entt::registry&, entt::entity e) {
    auto it = m_agents.find(e);
    if (it == m_agents.end()) return;
    if (m_crowd) m_crowd->removeAgent(it->second.agent);
    m_agents.erase(it);
}

void AIRuntime::onTreeChanged(entt::registry& r, entt::entity e) {
    auto it = m_trees.find(e);
    if (it == m_trees.end()) return;
    // Keep the running tree when only flags changed (e.g. a script toggled `enabled`).
    if (const auto* c = r.try_get<BehaviorTreeComponent>(e)) {
        if (it->second.treeId == c->tree && it->second.treeJson == c->treeJson) return;
    }
    m_trees.erase(it);
}

void AIRuntime::onTreeDestroyed(entt::registry&, entt::entity e) { m_trees.erase(e); }

void AIRuntime::onPerceptionDestroyed(entt::registry&, entt::entity e) {
    if (auto it = m_listeners.find(e); it != m_listeners.end()) {
        m_perception.removeListener(it->second);
        m_listeners.erase(it);
    }
    m_perception.removeSource(toRuntimeId(e));
}

void AIRuntime::onObstacleDestroyed(entt::registry&, entt::entity e) {
    auto it = m_obstacles.find(e);
    if (it == m_obstacles.end()) return;
    if (m_tileCache && it->second.id) m_tileCache->removeObstacle(it->second.id);
    m_obstacles.erase(it);
}

const ai::NavMesh* AIRuntime::activeMesh() const {
    if (m_tileCache) return &m_tileCache->navMesh();
    return m_mesh.get();
}

const ai::NavMesh* AIRuntime::navMesh() const { return activeMesh(); }

bool AIRuntime::bake(Entity surface) {
    if (!m_world || !surface.valid()) return false;
    auto* c = surface.tryGet<NavMeshSurfaceComponent>();
    if (!c) return false;
    OX_PROFILE_ZONE_N("AIRuntime::bake");
    auto* meshes = m_services ? m_services->tryGet<IMeshColliderProvider>() : nullptr;
    const ai::NavMeshInput input = gatherNavMeshInput(*m_world, *c, surface, meshes);
    if (input.indices.empty()) {
        OX_LOG_WARN("gameplay", "navmesh bake: no static geometry found");
        return false;
    }
    m_agents.clear();
    m_obstacles.clear();
    m_crowd.reset();
    m_query.reset();
    m_tileCache.reset();
    m_mesh.reset();
    if (c->dynamicObstacles) {
        ai::NavTileCacheSettings ts;
        ts.build = toBuildSettings(*c);
        m_tileCache = ai::NavTileCache::build(input, ts);
        if (!m_tileCache) return false;
        c->bakedData.clear();
    } else {
        m_mesh = ai::NavMesh::build(input, toBuildSettings(*c));
        if (!m_mesh) return false;
        c->bakedData = m_mesh->serialize();
    }
    c->baked = true;
    m_surfaceChecked = true;
    if (m_playing) createCrowd();
    return true;
}

bool AIRuntime::loadSurface(Entity surface, bool allowBake) {
    auto* c = surface.tryGet<NavMeshSurfaceComponent>();
    if (!c) return false;
    if (!c->bakedData.empty() && !c->dynamicObstacles) {
        m_mesh = ai::NavMesh::deserialize(c->bakedData);
        if (m_mesh) {
            c->baked = true;
            if (m_playing) createCrowd();
            return true;
        }
        OX_LOG_WARN("gameplay", "navmesh: stored data could not be loaded, rebaking");
    }
    return allowBake && bake(surface);
}

void AIRuntime::createCrowd() {
    const ai::NavMesh* mesh = activeMesh();
    if (!mesh) return;
    m_agents.clear();
    m_query = std::make_unique<ai::NavQuery>(*mesh);
    f32 maxRadius = 1.f;
    for (auto [e, a] : m_world->registry().view<NavAgentComponent>().each()) maxRadius = std::max(maxRadius, a.radius);
    m_crowd = std::make_unique<ai::NavCrowd>(*mesh, 256, maxRadius);
}

AIRuntime::AgentRecord* AIRuntime::ensureAgent(Entity e) {
    if (!m_crowd || !m_crowd->valid()) return nullptr;
    auto it = m_agents.find(e.handle());
    if (it != m_agents.end()) return it->second.agent ? &it->second : nullptr;
    auto& c = e.get<NavAgentComponent>();
    ai::NavAgentParams p;
    p.radius = c.radius;
    p.height = c.height;
    p.maxSpeed = c.maxSpeed;
    p.maxAcceleration = c.maxAcceleration;
    p.separationWeight = c.separationWeight;
    p.obstacleAvoidanceQuality = c.avoidanceQuality;
    AgentRecord rec;
    rec.agent = m_crowd->addAgent(e.worldPosition(), p);
    if (!rec.agent) OX_LOG_WARN("gameplay", "nav agent '{}' could not be added (off the navmesh?)", e.name());
    auto [ins, ok] = m_agents.emplace(e.handle(), rec);
    if (ins->second.agent && c.hasDestination) m_crowd->setTarget(ins->second.agent, c.destination);
    return ins->second.agent ? &ins->second : nullptr;
}

bool AIRuntime::moveTo(Entity e, const glm::vec3& destination) {
    auto* c = e.valid() ? e.tryGet<NavAgentComponent>() : nullptr;
    if (!c) return false;
    c->destination = destination;
    c->hasDestination = true;
    c->reached = false;
    if (AgentRecord* rec = ensureAgent(e)) return m_crowd->setTarget(rec->agent, destination);
    return activeMesh() == nullptr; // applied once the navmesh/agent exists
}

void AIRuntime::stop(Entity e) {
    auto* c = e.valid() ? e.tryGet<NavAgentComponent>() : nullptr;
    if (!c) return;
    c->hasDestination = false;
    auto it = m_agents.find(e.handle());
    if (it != m_agents.end() && it->second.agent && m_crowd) m_crowd->resetTarget(it->second.agent);
}

bool AIRuntime::reached(Entity e) const {
    const auto* c = e.valid() ? e.tryGet<NavAgentComponent>() : nullptr;
    return c && c->reached;
}

std::vector<glm::vec3> AIRuntime::findPath(const glm::vec3& from, const glm::vec3& to) const {
    if (!m_query) return {};
    return m_query->findPath(from, to).points;
}

void AIRuntime::reportNoise(const glm::vec3& position, f32 loudness, f32 radius, Entity instigator,
                            const std::string& tag) {
    ai::NoiseEvent n;
    n.position = position;
    n.loudness = loudness;
    n.radius = radius;
    n.instigator = toRuntimeId(instigator);
    if (const auto* p = instigator.valid() ? instigator.tryGet<PerceptionComponent>() : nullptr) n.team = p->team;
    n.tag = tag;
    m_perception.reportNoise(n);
}

void AIRuntime::updateObstacles() {
    if (!m_tileCache) return;
    for (auto [handle, o] : m_world->registry().view<NavObstacleComponent>().each()) {
        const Entity e = m_world->wrap(handle);
        const glm::vec3 pos = e.worldPosition();
        auto it = m_obstacles.find(handle);
        if (it != m_obstacles.end() && glm::distance(it->second.position, pos) < o.moveThreshold) continue;
        if (it != m_obstacles.end() && it->second.id) m_tileCache->removeObstacle(it->second.id);
        ObstacleRecord rec;
        rec.position = pos;
        rec.id = o.shape == NavObstacleShape::Cylinder
                     ? m_tileCache->addCylinder(pos, o.radius, o.height)
                     : m_tileCache->addOrientedBox(pos + glm::vec3(0, o.halfExtents.y, 0), o.halfExtents,
                                                   glm::eulerAngles(e.worldRotation()).y);
        m_obstacles[handle] = rec;
    }
    m_tileCache->update(0.f);
}

void AIRuntime::updateNavigation(f32 dt) {
    if (!m_world || !m_playing) return;
    OX_PROFILE_ZONE_N("AIRuntime::updateNavigation");
    entt::registry& r = m_world->registry();
    if (!activeMesh() && !m_surfaceChecked) {
        m_surfaceChecked = true;
        for (auto [handle, s] : r.view<NavMeshSurfaceComponent>().each()) {
            loadSurface(m_world->wrap(handle), s.bakeOnStart);
            break;
        }
    }
    if (!m_crowd) return;
    updateObstacles();

    auto view = r.view<NavAgentComponent>();
    for (auto handle : view) {
        const Entity e = m_world->wrap(handle);
        if (!e.activeInHierarchy()) continue;
        AgentRecord* rec = ensureAgent(e);
        if (!rec) continue;
        auto& c = view.get<NavAgentComponent>(handle);
        if (c.driveCharacterController && e.has<CharacterControllerComponent>()) {
            // The character controller moves the entity; keep the crowd agent from drifting away from it.
            glm::vec3 d = m_crowd->position(rec->agent) - e.worldPosition();
            d.y = 0.f;
            if (glm::length(d) > c.radius * 2.f) {
                m_crowd->removeAgent(rec->agent);
                m_agents.erase(handle);
                rec = ensureAgent(e);
                if (!rec) continue;
            }
        }
    }
    m_crowd->update(dt);
    for (auto handle : view) {
        auto it = m_agents.find(handle);
        if (it == m_agents.end() || !it->second.agent) continue;
        const Entity e = m_world->wrap(handle);
        auto& c = view.get<NavAgentComponent>(handle);
        const ai::NavAgent agent = it->second.agent;
        const glm::vec3 vel = m_crowd->velocity(agent);
        c.velocity = vel;
        c.reached = c.hasDestination && m_crowd->reachedTarget(agent, c.stoppingDistance);
        const glm::vec3 flat(vel.x, 0.f, vel.z);
        if (c.driveCharacterController) {
            if (auto* cc = e.tryGet<CharacterControllerComponent>()) {
                cc->desiredVelocity = c.reached ? glm::vec3(0.f) : flat;
                if (c.updateRotation && glm::length2(flat) > 0.01f) e.setWorldRotation(lookRotation(glm::normalize(flat)));
                continue;
            }
        }
        if (c.updatePosition) e.setWorldPosition(m_crowd->position(agent));
        if (c.updateRotation && glm::length2(flat) > 0.01f) e.setWorldRotation(lookRotation(glm::normalize(flat)));
    }
}

AIRuntime::TreeRecord* AIRuntime::ensureTree(Entity e) {
    auto it = m_trees.find(e.handle());
    if (it != m_trees.end()) return it->second.failed ? nullptr : &it->second;
    TreeRecord rec;
    const auto& c = e.get<BehaviorTreeComponent>();
    rec.treeId = c.tree;
    rec.treeJson = c.treeJson;
    std::optional<json> def;
    if (c.tree.isValid()) {
        if (auto* p = m_services ? m_services->tryGet<IBehaviorTreeProvider>() : nullptr) def = p->behaviorTree(c.tree);
        if (!def) OX_LOG_WARN("gameplay", "behaviour tree {} of '{}' not found", c.tree.toString(), e.name());
    } else if (!c.treeJson.empty()) {
        try {
            def = json::parse(c.treeJson);
        } catch (const std::exception& ex) {
            OX_LOG_WARN("gameplay", "behaviour tree JSON of '{}': {}", e.name(), ex.what());
        }
    }
    if (def) {
        try {
            rec.tree = m_factory.load(*def);
        } catch (const std::exception& ex) {
            OX_LOG_WARN("gameplay", "behaviour tree of '{}': {}", e.name(), ex.what());
        }
    }
    if (!rec.tree) {
        rec.failed = true;
        m_trees.emplace(e.handle(), std::move(rec));
        return nullptr;
    }
    ai::Blackboard& bb = rec.tree->blackboard();
    for (const BlackboardEntry& entry : c.blackboard) {
        switch (entry.type) {
        case BlackboardEntryType::Bool: bb.set(entry.key, entry.boolValue); break;
        case BlackboardEntryType::Int: bb.set(entry.key, entry.intValue); break;
        case BlackboardEntryType::Float: bb.set(entry.key, entry.floatValue); break;
        case BlackboardEntryType::String: bb.set(entry.key, entry.stringValue); break;
        case BlackboardEntryType::Vec3: bb.set(entry.key, entry.vec3Value); break;
        case BlackboardEntryType::Entity:
            if (Entity t = m_world->resolve(entry.entityValue); t.valid()) bb.set(entry.key, toBlackboardId(t));
            break;
        }
    }
    bb.set("self", toBlackboardId(e));
    rec.context = std::make_unique<BTAgentContext>(BTAgentContext{this, m_world, e.handle()});
    rec.tree->setUserData(rec.context.get());
    auto [ins, ok] = m_trees.emplace(e.handle(), std::move(rec));
    return &ins->second;
}

u32 AIRuntime::reloadBehaviorTree(const Uuid& id) {
    if (!m_world || !id.isValid()) return 0;
    std::vector<std::pair<entt::entity, std::unordered_map<std::string, ai::BlackboardValue>>> affected;
    for (auto& [e, rec] : m_trees) {
        if (rec.treeId != id) continue;
        affected.emplace_back(e, rec.tree ? rec.tree->blackboard().values()
                                          : std::unordered_map<std::string, ai::BlackboardValue>{});
    }
    u32 rebuilt = 0;
    for (auto& [e, values] : affected) {
        m_trees.erase(e);
        if (!m_world->valid(e) || !m_world->registry().all_of<BehaviorTreeComponent>(e)) continue;
        TreeRecord* rec = ensureTree(m_world->wrap(e));
        if (!rec) continue;
        ai::Blackboard& bb = rec->tree->blackboard();
        for (auto& [key, value] : values) bb.setValue(key, std::move(value));
        ++rebuilt;
    }
    if (rebuilt) OX_LOG_INFO("gameplay", "behaviour tree {} reloaded ({} instance(s))", id.toString(), rebuilt);
    return rebuilt;
}

void AIRuntime::updateBehaviorTrees(f32 dt) {
    if (!m_world || !m_playing) return;
    OX_PROFILE_ZONE_N("AIRuntime::updateBehaviorTrees");
    entt::registry& r = m_world->registry();
    std::vector<entt::entity> entities(r.view<BehaviorTreeComponent>().begin(), r.view<BehaviorTreeComponent>().end());
    for (auto handle : entities) {
        if (!r.valid(handle) || r.all_of<PendingDestroyTag>(handle)) continue;
        const Entity e = m_world->wrap(handle);
        auto* c = &r.get<BehaviorTreeComponent>(handle);
        if (!c->enabled || !e.activeInHierarchy()) continue;
        TreeRecord* rec = ensureTree(e);
        if (!rec) continue;
        if (!c->restartOnFinish && (c->status == ai::BTStatus::Success || c->status == ai::BTStatus::Failure)) continue;
        rec->accumulator += dt;
        if (c->tickInterval > 0.f && rec->accumulator < c->tickInterval) continue;
        const f32 tickDt = rec->accumulator;
        rec->accumulator = 0.f;
        const ai::BTStatus status = rec->tree->tick(tickDt);
        if (auto* still = r.try_get<BehaviorTreeComponent>(handle)) still->status = status;
    }
}

void AIRuntime::updatePerception(f32 dt) {
    if (!m_world || !m_playing) return;
    entt::registry& r = m_world->registry();
    auto view = r.view<PerceptionComponent>();
    if (view.empty()) return;
    OX_PROFILE_ZONE_N("AIRuntime::updatePerception");
    m_perceptionEvents.clear();
    for (auto handle : view) {
        const Entity e = m_world->wrap(handle);
        const auto& c = view.get<PerceptionComponent>(handle);
        const Transform wt = e.worldTransform();
        const u64 id = toRuntimeId(e);
        if (c.source && e.activeInHierarchy()) {
            m_perception.setSource({id, wt.position, c.team, c.visible});
        } else {
            m_perception.removeSource(id);
        }
        if (!c.listener) continue;
        auto it = m_listeners.find(handle);
        if (it == m_listeners.end()) {
            ai::PerceptionListenerDesc d;
            d.selfId = id;
            it = m_listeners.emplace(handle, m_perception.addListener(d)).first;
        }
        if (ai::PerceptionListenerDesc* d = m_perception.listener(it->second)) {
            d->team = c.team;
            d->sight = c.sight;
            d->hearing = c.hearing;
            d->detectHostile = c.detectHostile;
            d->detectNeutral = c.detectNeutral;
            d->detectFriendly = c.detectFriendly;
        }
        m_perception.setListenerTransform(it->second, wt.position, wt.forward());
    }
    m_perception.update(dt);
    for (auto& [handle, lid] : m_listeners) {
        if (!r.valid(handle)) continue;
        auto* c = r.try_get<PerceptionComponent>(handle);
        if (!c) continue;
        const auto best = m_perception.bestHostile(lid);
        const Entity target = best ? entityFromRuntimeId(*m_world, best->sourceId) : Entity{};
        c->target = target.valid() ? target.ref() : EntityRef{};
        c->targetVisible = target.valid() && best->sense == ai::Sense::Sight && best->currentlySensed;
        if (auto tit = m_trees.find(handle); tit != m_trees.end() && tit->second.tree) {
            ai::Blackboard& bb = tit->second.tree->blackboard();
            if (target.valid()) {
                bb.set("target", toBlackboardId(target));
                bb.set("targetPosition", best->lastKnownPosition);
            } else {
                bb.erase("target");
            }
            bb.set("targetVisible", c->targetVisible);
        }
    }
    for (const auto& ev : m_perceptionEvents) {
        onPerception.emit(ev);
        if (m_bus) m_bus->publish(ev);
    }
}

ai::BehaviorTree* AIRuntime::behaviorTree(Entity e) const {
    auto it = e.valid() ? m_trees.find(e.handle()) : m_trees.end();
    return it == m_trees.end() ? nullptr : it->second.tree.get();
}

ai::Blackboard* AIRuntime::blackboard(Entity e) const {
    ai::BehaviorTree* t = behaviorTree(e);
    return t ? &t->blackboard() : nullptr;
}

void AIRuntime::drawDebug(DebugDraw& draw, bool playing) {
    if (!m_world) return;
    auto line = [&](glm::vec3 a, glm::vec3 b, glm::vec4 c) { draw.line(a, b, c); };
    if (!playing) {
        // Editor: show the stored navmesh of surfaces that ask for it.
        for (auto [handle, s] : m_world->registry().view<NavMeshSurfaceComponent>().each()) {
            if (!s.drawInEditor) continue;
            if (!m_mesh && !s.bakedData.empty()) m_mesh = ai::NavMesh::deserialize(s.bakedData);
            if (m_mesh) m_mesh->debugDraw(line);
            break;
        }
        return;
    }
    if (!debugDraw) return;
    if (const ai::NavMesh* mesh = activeMesh()) mesh->debugDraw(line);
    if (m_tileCache) m_tileCache->debugDraw(line);
    if (m_crowd) m_crowd->debugDraw(line);
    for (auto& [e, id] : m_listeners) m_perception.debugDraw(id, line);
}

} // namespace ox::gameplay
