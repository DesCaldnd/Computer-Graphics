#include <oxwald/core/cvar.hpp>
#include <oxwald/core/debug_draw.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/core/profile.hpp>
#include <oxwald/gameplay/audio.hpp>
#include <oxwald/gameplay/destruction.hpp>
#include <oxwald/gameplay/physics.hpp>
#include <oxwald/gameplay/providers.hpp>
#include <oxwald/gameplay/script.hpp>
#include <oxwald/scene/components.hpp>

#include <glm/gtx/norm.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <optional>

namespace ox::gameplay {

namespace {

CVar<int> cvMaxDebris("destruction.MaxActiveDebris", 256,
                      "Global budget of moving destruction chunks; the oldest fade out when it is exceeded",
                      Scalability::Effects, {64, 128, 256, 512});
CVar<float> cvLifetimeScale("destruction.DebrisLifetimeScale", 1.f,
                            "Multiplier of DestructibleComponent::debrisLifetime", Scalability::Effects,
                            {0.4f, 0.7f, 1.f, 1.5f});
CVar<bool> cvEnabled("destruction.Enabled", true, "Destructibles break (off: damage is still tracked, nothing breaks)");

constexpr f32 kBudgetFade = 0.3f;

f32 maxAbs3(const glm::vec3& v) { return std::max({std::abs(v.x), std::abs(v.y), std::abs(v.z)}); }

std::optional<physics::FractureMesh> fromTriangles(const MeshTriangles& t) {
    physics::FractureMesh m;
    m.positions = t.vertices;
    m.indices = t.indices;
    if (!physics::isClosedMesh(m.positions, m.indices) || physics::meshVolume(m.positions, m.indices) <= 0.f) {
        m = physics::makeConvexHullFractureMesh(t.vertices);
    }
    if (m.indices.empty()) return std::nullopt;
    return m;
}

std::vector<glm::vec3> roundPoints(f32 radius, f32 halfHeight, bool capsule, bool cylinder) {
    std::vector<glm::vec3> pts;
    constexpr u32 kSegments = 16;
    if (cylinder) {
        for (u32 i = 0; i < kSegments; ++i) {
            const f32 a = 6.2831853f * f32(i) / f32(kSegments);
            pts.push_back({std::cos(a) * radius, -halfHeight, std::sin(a) * radius});
            pts.push_back({std::cos(a) * radius, halfHeight, std::sin(a) * radius});
        }
        return pts;
    }
    constexpr u32 kRings = 8;
    for (u32 r = 0; r <= kRings; ++r) {
        const f32 phi = 3.14159265f * f32(r) / f32(kRings);
        const f32 y = std::cos(phi) * radius;
        const f32 ring = std::sin(phi) * radius;
        const f32 offset = capsule ? (r <= kRings / 2 ? halfHeight : -halfHeight) : 0.f;
        const u32 count = (r == 0 || r == kRings) ? 1 : kSegments;
        for (u32 i = 0; i < count; ++i) {
            const f32 a = 6.2831853f * f32(i) / f32(kSegments);
            pts.push_back({std::cos(a) * ring, y + offset, std::sin(a) * ring});
            if (capsule && r == kRings / 2) pts.push_back({std::cos(a) * ring, y - offset, std::sin(a) * ring});
        }
    }
    return pts;
}

// Fracture data is in entity space with the world scale already applied (Voronoi cells in metres, chunk entities
// have unit scale), so only rotation + translation map it to the world.
glm::vec3 worldPoint(const Transform& t, const glm::vec3& local) { return t.position + t.rotation * local; }

void applyScale(physics::FractureMesh& m, const glm::vec3& scale) {
    if (scale == glm::vec3(1.f)) return;
    for (auto& p : m.positions) p *= scale;
    for (auto& n : m.normals) n = glm::normalize(n / scale);
}

void worldBounds(const Transform& t, const glm::vec3& lmin, const glm::vec3& lmax, glm::vec3& mn, glm::vec3& mx) {
    mn = glm::vec3(std::numeric_limits<f32>::max());
    mx = glm::vec3(std::numeric_limits<f32>::lowest());
    for (int k = 0; k < 8; ++k) {
        const glm::vec3 corner((k & 1) ? lmax.x : lmin.x, (k & 2) ? lmax.y : lmin.y, (k & 4) ? lmax.z : lmin.z);
        const glm::vec3 w = worldPoint(t, corner);
        mn = glm::min(mn, w);
        mx = glm::max(mx, w);
    }
}

physics::FractureSettings settingsOf(const DestructibleComponent& d) {
    physics::FractureSettings s;
    s.chunkCount = std::clamp(d.chunkCount, 1u, 256u);
    s.seed = d.seed;
    s.interiorUvScale = d.interiorUvScale;
    s.planarRatio = d.planarRatio;
    return s;
}

} // namespace

DestructionRuntime::DestructionRuntime() = default;
DestructionRuntime::~DestructionRuntime() { detach(); }

// ---- attach / play state ---------------------------------------------------------------------------------------

void DestructionRuntime::attach(World& world, Services& services) {
    detach();
    m_world = &world;
    m_services = &services;
    m_physics = services.tryGet<PhysicsRuntime>();
    m_bus = services.tryGet<EventBus>();
    if (m_physics) m_collisionConnection = m_physics->onCollision.connect([this](const CollisionEvent& ev) { onCollision(ev); });
    entt::registry& r = world.registry();
    m_connections.emplace_back(r.on_update<DestructibleComponent>().connect<&DestructionRuntime::onDestructibleChanged>(*this));
    m_connections.emplace_back(r.on_construct<DestructibleComponent>().connect<&DestructionRuntime::onDestructibleChanged>(*this));
}

void DestructionRuntime::detach() {
    if (!m_world) return;
    syncPlayState(false);
    m_collisionConnection = {};
    m_connections.clear();
    m_world = nullptr;
    m_services = nullptr;
    m_physics = nullptr;
    m_bus = nullptr;
}

void DestructionRuntime::syncPlayState(bool playing) {
    if (playing == m_playing) return;
    m_playing = playing;
    if (!m_world) return;
    if (playing) {
        // Fracture up front: chunk meshes reach the renderer long before anything breaks.
        std::vector<entt::entity> all;
        for (auto e : m_world->registry().view<DestructibleComponent>()) all.push_back(e);
        std::sort(all.begin(), all.end());
        for (auto e : all) prepare(m_world->wrap(e));
        for (auto e : all) {
            const auto& d = m_world->registry().get<DestructibleComponent>(e);
            if (d.structural && d.anchor == DestructionAnchor::Static) recordSupports(m_world->wrap(e), m_owners[e]);
        }
        return;
    }
    // Chunks are runtime entities; play-in-editor discards the cloned world anyway.
    for (auto& [e, rec] : m_chunks) {
        if (m_world->valid(e)) m_world->destroy(m_world->wrap(e));
    }
    m_chunks.clear();
    m_owners.clear();
    m_contacts.clear();
}

void DestructionRuntime::onDestructibleChanged(entt::registry&, entt::entity e) {
    auto it = m_owners.find(e);
    if (it != m_owners.end() && !it->second.fractured) it->second.fracture.reset();
}

void DestructionRuntime::onCollision(const CollisionEvent& ev) {
    if (!m_playing || ev.phase != ContactPhase::Begin || ev.impulse <= 0.f) return;
    m_contacts.push_back({ev.a.valid() ? ev.a.handle() : entt::null, ev.b.valid() ? ev.b.handle() : entt::null, ev.point,
                          ev.normal, ev.impulse});
}

// ---- fracture source / preparation -----------------------------------------------------------------------------

std::optional<physics::FractureMesh> DestructionRuntime::sourceMeshOf(Entity e) const {
    if (!e.valid()) return std::nullopt;
    const auto* d = e.tryGet<DestructibleComponent>();
    if (!d) return std::nullopt;
    IMeshColliderProvider* provider = m_services ? m_services->tryGet<IMeshColliderProvider>() : nullptr;
    if (d->sourceMesh.isValid()) {
        if (auto tris = provider ? provider->meshTriangles(d->sourceMesh) : nullptr) {
            auto m = fromTriangles(*tris);
            if (m) applyScale(*m, glm::abs(e.worldTransform().scale));
            return m;
        }
        OX_LOG_WARN("gameplay", "destructible '{}': source mesh {} not available", e.name(), d->sourceMesh.toString());
    }
    const auto* col = e.tryGet<ColliderComponent>();
    if (!col) return std::nullopt;
    std::optional<physics::FractureMesh> m;
    switch (col->type) {
    case ColliderType::Box: m = physics::makeBoxFractureMesh(col->halfExtents); break;
    case ColliderType::Sphere: m = physics::makeConvexHullFractureMesh(roundPoints(col->radius, 0.f, false, false)); break;
    case ColliderType::Capsule:
        m = physics::makeConvexHullFractureMesh(roundPoints(col->radius, col->halfHeight, true, false));
        break;
    case ColliderType::Cylinder:
        m = physics::makeConvexHullFractureMesh(roundPoints(col->radius, col->halfHeight, false, true));
        break;
    case ColliderType::ConvexHull:
    case ColliderType::Mesh:
        if (col->type == ColliderType::ConvexHull && !col->points.empty()) {
            m = physics::makeConvexHullFractureMesh(col->points);
        } else if (auto tris = col->mesh.isValid() && provider ? provider->meshTriangles(col->mesh) : nullptr) {
            if (col->type == ColliderType::Mesh) m = fromTriangles(*tris);
            else m = physics::makeConvexHullFractureMesh(tris->vertices);
        }
        break;
    default: break;
    }
    if (!m || m->indices.empty()) {
        OX_LOG_WARN("gameplay", "destructible '{}': collider type cannot be fractured", e.name());
        return std::nullopt;
    }
    // Collider scale and offset, then the entity's world scale.
    const glm::vec3 s = col->scale;
    const glm::quat q = glm::normalize(col->offsetRotation);
    const bool identity = s == glm::vec3(1.f) && col->offsetPosition == glm::vec3(0.f) && q == glm::quat(1, 0, 0, 0);
    if (!identity) {
        for (auto& p : m->positions) p = col->offsetPosition * s + q * (p * s);
        for (auto& n : m->normals) n = glm::normalize(q * (n / s));
    }
    applyScale(*m, glm::abs(e.worldTransform().scale));
    return m;
}

std::shared_ptr<const PreparedFracture> DestructionRuntime::prepare(Entity e) {
    if (!e.valid() || !e.has<DestructibleComponent>()) return nullptr;
    OwnerRecord& owner = m_owners[e.handle()];
    if (owner.fracture) return owner.fracture;
    OX_PROFILE_ZONE_N("Destruction::prepare");
    const auto& d = e.get<DestructibleComponent>();
    auto mesh = sourceMeshOf(e);
    if (!mesh) return nullptr;
    const physics::FractureSettings settings = settingsOf(d);
    const u64 key = physics::fractureKey(*mesh, settings);
    if (auto it = m_cache.find(key); it != m_cache.end()) {
        owner.fracture = it->second;
        e.get<DestructibleComponent>().prepared = owner.fracture;
        return owner.fracture;
    }
    auto prepared = std::make_shared<PreparedFracture>();
    prepared->key = key;
    prepared->result = std::make_shared<const physics::FractureResult>(physics::voronoiFracture(*mesh, settings));
    if (prepared->result->empty()) {
        OX_LOG_WARN("gameplay", "destructible '{}': fracture produced no chunks", e.name());
        return nullptr;
    }
    for (u32 i = 0; i < prepared->result->chunks.size(); ++i) {
        prepared->meshes.push_back(Uuid::fromName(std::format("ox.destruction/{:016x}/{}", key, i)));
    }
    registerMeshes(*prepared);
    m_cache.emplace(key, prepared);
    owner.fracture = prepared;
    e.get<DestructibleComponent>().prepared = prepared;
    OX_LOG_INFO("gameplay", "destructible '{}': {} chunks, {} links", e.name(), prepared->result->chunks.size(),
                prepared->result->links.size());
    return prepared;
}

void DestructionRuntime::registerMeshes(const PreparedFracture& f) {
    if (m_meshRegistered.contains(f.key)) return;
    m_meshRegistered[f.key] = true;
    for (u32 i = 0; i < f.meshes.size(); ++i) m_meshList.push_back({f.meshes[i], f.result, i});
    ++m_meshVersion;
}

// ---- queries -----------------------------------------------------------------------------------------------------

bool DestructionRuntime::isBroken(Entity e) const {
    if (!e.valid()) return false;
    if (const auto* d = e.tryGet<DestructibleComponent>()) return d->broken;
    return false;
}

std::vector<Entity> DestructionRuntime::chunksOf(Entity destructible) const {
    std::vector<Entity> out;
    if (!m_world || !destructible.valid()) return out;
    auto it = m_owners.find(destructible.handle());
    if (it == m_owners.end()) return out;
    for (entt::entity c : it->second.chunks) {
        if (c != entt::null && m_world->valid(c) && m_chunks.contains(c)) out.push_back(m_world->wrap(c));
    }
    return out;
}

u32 DestructionRuntime::activeDebrisCount() const {
    u32 n = 0;
    for (const auto& [e, rec] : m_chunks) n += (!rec.frozen && !rec.fading) ? 1u : 0u;
    return n;
}

u32 DestructionRuntime::frozenChunkCount(Entity destructible) const {
    u32 n = 0;
    for (const auto& [e, rec] : m_chunks) n += (rec.frozen && rec.owner == destructible.handle()) ? 1u : 0u;
    return n;
}

u32 DestructionRuntime::fadingCount() const {
    u32 n = 0;
    for (const auto& [e, rec] : m_chunks) n += rec.fading ? 1u : 0u;
    return n;
}

u32 DestructionRuntime::budget() const {
    return m_maxDebrisOverride ? m_maxDebrisOverride : u32(std::max(cvMaxDebris.get(), 0));
}

// ---- damage ------------------------------------------------------------------------------------------------------

bool DestructionRuntime::applyDamage(Entity e, f32 amount, const glm::vec3& point, const glm::vec3& impulse,
                                     DestructionCause cause) {
    if (!m_playing || !m_world || !e.valid()) return false;
    if (auto it = m_chunks.find(e.handle()); it != m_chunks.end()) {
        ChunkRecord& rec = it->second;
        if (!rec.frozen || !m_world->valid(rec.owner)) {
            if (m_physics && !rec.fading && glm::length2(impulse) > 0.f) m_physics->addImpulseAtPoint(e, impulse, point);
            return false;
        }
        const auto* d = m_world->registry().try_get<DestructibleComponent>(rec.owner);
        if (d && amount < d->damageThreshold) return false;
        // The hit chunk plus its frozen neighbours within the break radius.
        const f32 radius = d ? d->breakRadius : 0.f;
        std::vector<u32> release{rec.index};
        const OwnerRecord& owner = m_owners[rec.owner];
        for (entt::entity c : owner.chunks) {
            auto ct = c != entt::null ? m_chunks.find(c) : m_chunks.end();
            if (ct == m_chunks.end() || !ct->second.frozen || c == e.handle()) continue;
            if (glm::distance(m_world->wrap(c).worldPosition(), point) <= radius) release.push_back(ct->second.index);
        }
        return releaseChunks(rec.owner, release, point, impulse, cause, false);
    }
    auto* d = e.tryGet<DestructibleComponent>();
    if (!d || d->broken) return false;
    if (amount < d->damageThreshold || amount <= 0.f) return false;
    d->health -= amount;
    if (d->health > 0.f) return false;
    d->health = 0.f;
    return fracture(e, point, impulse, cause);
}

u32 DestructionRuntime::applyRadialDamage(const glm::vec3& center, f32 radius, f32 damage, f32 impulse) {
    if (!m_playing || !m_world || radius <= 0.f) return 0;
    std::vector<entt::entity> targets;
    for (auto e : m_world->registry().view<DestructibleComponent>()) targets.push_back(e);
    for (const auto& [e, rec] : m_chunks) targets.push_back(e);
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    u32 affected = 0;
    for (auto h : targets) {
        if (!m_world->valid(h)) continue;
        const Entity e = m_world->wrap(h);
        glm::vec3 p = e.worldPosition();
        // Intact objects: distance to their bounds (a blast at the foot of a tower reaches it).
        if (!m_chunks.contains(h)) {
            auto ot = m_owners.find(h);
            if (ot != m_owners.end() && ot->second.fracture && !ot->second.fractured) {
                glm::vec3 mn, mx;
                worldBounds(e.worldTransform(), ot->second.fracture->result->boundsMin, ot->second.fracture->result->boundsMax, mn, mx);
                p = glm::clamp(center, mn, mx);
            }
        }
        const f32 dist = glm::distance(p, center);
        if (dist > radius) continue;
        const f32 falloff = 1.f - dist / radius;
        const glm::vec3 dir = dist > 1e-4f ? (p - center) / dist : glm::vec3(0.f, 1.f, 0.f);
        const glm::vec3 j = dir * impulse * falloff;
        auto rt = m_chunks.find(h);
        if (rt != m_chunks.end() && !rt->second.frozen) {
            if (m_physics && !rt->second.fading && impulse > 0.f) m_physics->addImpulse(e, j);
            ++affected;
            continue;
        }
        if (auto* d = e.tryGet<DestructibleComponent>(); d && !d->broken) {
            // A blast that breaks an intact structure knocks out everything inside the blast radius.
            const f32 amount = damage * falloff;
            if (amount < d->damageThreshold || amount <= 0.f) continue;
            d->health -= amount;
            if (d->health > 0.f) continue;
            d->health = 0.f;
            if (fractureAt(e, center, j, DestructionCause::Damage, radius)) ++affected;
            continue;
        }
        if (applyDamage(e, damage * falloff, center, j, DestructionCause::Damage)) ++affected;
    }
    return affected;
}

// ---- breaking ----------------------------------------------------------------------------------------------------

Entity DestructionRuntime::spawnChunk(Entity intact, const DestructibleComponent& d,
                                      const std::shared_ptr<const PreparedFracture>& fp, u32 index, bool frozen,
                                      const glm::vec3& linear, const glm::vec3& angular) {
    const PreparedFracture& f = *fp;
    const physics::FractureChunk& c = f.result->chunks[index];
    const Transform wt = intact.worldTransform();
    Entity e = m_world->create(std::format("{}.Chunk{}", intact.name(), index));
    Transform t;
    t.position = worldPoint(wt, c.centroid);
    t.rotation = glm::normalize(wt.rotation);
    t.scale = glm::vec3(1.f); // the fracture already contains the intact entity's scale
    e.setLocalTransform(t);

    // Copies: adding components to the new entity must not be able to invalidate pointers into the source's.
    const std::optional<RigidBodyComponent> srcBody =
        intact.has<RigidBodyComponent>() ? std::optional(intact.get<RigidBodyComponent>()) : std::nullopt;
    const std::optional<MeshRendererComponent> srcMr =
        intact.has<MeshRendererComponent>() ? std::optional(intact.get<MeshRendererComponent>()) : std::nullopt;
    const f32 density = d.density > 0.f ? d.density : (intact.has<ColliderComponent>() ? intact.get<ColliderComponent>().density : 1000.f);

    auto& col = e.add<ColliderComponent>();
    col.type = ColliderType::ConvexHull;
    col.points = c.hullPoints;
    col.density = density;
    auto& rb = e.add<RigidBodyComponent>();
    rb.motionType = frozen ? physics::MotionType::Static : physics::MotionType::Dynamic;
    rb.layer = d.debrisLayer;
    rb.mass = 0.f; // from density and the hull volume
    if (srcBody) {
        rb.friction = srcBody->friction;
        rb.restitution = srcBody->restitution;
        rb.linearDamping = srcBody->linearDamping;
        rb.angularDamping = srcBody->angularDamping;
    }
    rb.initialLinearVelocity = linear;
    rb.initialAngularVelocity = angular;

    if (const MeshRendererComponent* mr = srcMr ? &*srcMr : nullptr) {
        auto& out = e.add<MeshRendererComponent>();
        out.mesh = f.meshes[index];
        const Uuid outer = mr->materials.empty() ? Uuid{} : mr->materials[0];
        out.materials = {outer, d.interiorMaterial.isValid() ? d.interiorMaterial : outer};
        if (c.interiorIndexStart == 0) out.materials = {out.materials[1]}; // interior-only chunk: one submesh
        out.castShadows = mr->castShadows;
        out.receiveShadows = mr->receiveShadows;
        out.visible = mr->visible;
        out.layerMask = mr->layerMask;
    }
    auto& marker = e.add<DestructionChunkComponent>();
    marker.owner = intact.ref();
    marker.index = index;
    marker.frozen = frozen;
    marker.fracture = fp;

    ChunkRecord rec;
    rec.entity = e.handle();
    rec.owner = intact.handle();
    rec.index = index;
    rec.volume = c.volume;
    rec.frozen = frozen;
    rec.order = ++m_spawnCounter;
    rec.lifetime = d.debrisLifetime;
    rec.fadeDuration = d.fadeDuration;
    rec.ownerLimit = d.maxActiveDebris;
    m_chunks[e.handle()] = rec;
    return e;
}

bool DestructionRuntime::fracture(Entity e, const glm::vec3& point, const glm::vec3& impulse, DestructionCause cause) {
    return fractureAt(e, point, impulse, cause, 0.f);
}

bool DestructionRuntime::fractureAt(Entity e, const glm::vec3& point, const glm::vec3& impulse, DestructionCause cause,
                                    f32 releaseRadius) {
    if (!m_playing || !m_world || !e.valid() || !cvEnabled.get()) return false;
    auto* dp = e.tryGet<DestructibleComponent>();
    if (!dp) return false;
    OwnerRecord& owner = m_owners[e.handle()];
    if (owner.fractured) return false;
    OX_PROFILE_ZONE_N("Destruction::fracture");
    auto prepared = prepare(e);
    DestructibleComponent& d = *dp;
    d.broken = true;
    d.health = std::min(d.health, 0.f);
    owner.fractured = true;
    if (!prepared) {
        DestructionEvent ev{DestructionEvent::Type::Fractured, e, cause, point, impulse, 0, 0};
        emit(ev);
        return true;
    }
    const physics::FractureResult& fr = *prepared->result;
    const Transform wt = e.worldTransform();
    const u32 n = u32(fr.chunks.size());

    // Motion inherited from the intact body.
    glm::vec3 linear{0.f}, angular{0.f};
    bool dynamicSource = false;
    if (const auto* rb = e.tryGet<RigidBodyComponent>(); rb && rb->motionType == physics::MotionType::Dynamic) {
        dynamicSource = true;
        if (m_physics) {
            linear = m_physics->linearVelocity(e);
            angular = m_physics->angularVelocity(e);
        }
    }
    const glm::vec3 com = worldPoint(wt, fr.sourceCentroid);
    std::vector<glm::vec3> centers(n);
    for (u32 i = 0; i < n; ++i) centers[i] = worldPoint(wt, fr.chunks[i].centroid);

    // Structural: which chunks stay frozen.
    const bool structural = d.structural && !dynamicSource;
    std::vector<bool> release(n, true);
    std::vector<bool> anchored(n, false);
    u32 disconnected = 0; // released because they lost every path to an anchor (not hit)
    if (structural) {
        const f32 tol = std::max(d.anchorTolerance, 0.f);
        if (d.anchor == DestructionAnchor::Bottom) {
            for (u32 i = 0; i < n; ++i) anchored[i] = fr.chunks[i].boundsMin.y - fr.boundsMin.y <= tol;
        } else if (d.anchor == DestructionAnchor::Static && m_physics) {
            for (u32 i = 0; i < n; ++i) {
                glm::vec3 mn, mx;
                worldBounds(wt, fr.chunks[i].boundsMin, fr.chunks[i].boundsMax, mn, mx);
                anchored[i] = !staticSupports(mn, mx, e.handle(), tol).empty();
            }
        }
        // Chunks around the hit are knocked out (always at least the closest one).
        u32 closest = 0;
        f32 best = std::numeric_limits<f32>::max();
        for (u32 i = 0; i < n; ++i) {
            const f32 dist = glm::distance(centers[i], point);
            release[i] = dist <= std::max(d.breakRadius, releaseRadius);
            if (dist < best) best = dist, closest = i;
        }
        release[closest] = true;
        // Disconnected from every anchor -> collapses with the rest.
        const auto adj = fr.adjacency();
        std::vector<bool> held(n, false);
        std::vector<u32> stack;
        for (u32 i = 0; i < n; ++i) {
            if (anchored[i] && !release[i]) {
                held[i] = true;
                stack.push_back(i);
            }
        }
        while (!stack.empty()) {
            const u32 i = stack.back();
            stack.pop_back();
            for (u32 j : adj[i]) {
                if (!held[j] && !release[j]) {
                    held[j] = true;
                    stack.push_back(j);
                }
            }
        }
        for (u32 i = 0; i < n; ++i) {
            disconnected += (!held[i] && !release[i]) ? 1u : 0u;
            release[i] = !held[i];
        }
    }

    // Impact push: shared by the released chunks near the hit (linear falloff over breakRadius).
    std::vector<glm::vec3> push(n, glm::vec3(0.f));
    const f32 jLen = glm::length(impulse);
    if (jLen > 0.f && d.impactImpulseScale > 0.f) {
        const f32 radius = std::max(d.breakRadius, 1e-3f);
        std::vector<f32> w(n, 0.f);
        f32 total = 0.f;
        u32 closest = 0;
        f32 best = std::numeric_limits<f32>::max();
        for (u32 i = 0; i < n; ++i) {
            if (!release[i]) continue;
            const f32 dist = glm::distance(centers[i], point);
            if (dist < best) best = dist, closest = i;
            w[i] = std::max(0.f, 1.f - dist / radius);
            total += w[i];
        }
        if (total <= 0.f && release[closest]) w[closest] = total = 1.f;
        const glm::vec3 dir = impulse / jLen;
        for (u32 i = 0; i < n && total > 0.f; ++i) {
            if (w[i] <= 0.f) continue;
            const glm::vec3 radial = glm::length2(centers[i] - point) > 1e-8f ? glm::normalize(centers[i] - point) : dir;
            const glm::vec3 pdir = glm::normalize(dir + radial * d.spread);
            const f32 density = d.density > 0.f ? d.density : (e.has<ColliderComponent>() ? e.get<ColliderComponent>().density : 1000.f);
            const f32 mass = std::max(density * fr.chunks[i].volume, 1e-3f);
            push[i] = pdir * (jLen * d.impactImpulseScale * w[i] / total) / mass;
        }
    }

    owner.chunks.assign(n, entt::null);
    u32 moving = 0;
    for (u32 i = 0; i < n; ++i) {
        const bool frozen = !release[i];
        glm::vec3 v = linear + glm::cross(angular, centers[i] - com) + push[i];
        Entity c = spawnChunk(e, d, prepared, i, frozen, frozen ? glm::vec3(0.f) : v, frozen ? glm::vec3(0.f) : angular);
        owner.chunks[i] = c.handle();
        m_chunks[c.handle()].anchored = anchored[i];
        c.get<DestructionChunkComponent>().anchored = anchored[i];
        moving += frozen ? 0u : 1u;
    }

    // Swap: the intact entity loses its body and visuals (it stays alive for scripts, events and replication).
    if (e.has<RigidBodyComponent>()) e.remove<RigidBodyComponent>();
    if (e.has<ColliderComponent>()) e.remove<ColliderComponent>();
    if (e.has<MeshRendererComponent>()) e.remove<MeshRendererComponent>();

    OX_LOG_DEBUG("gameplay", "destructible '{}' broke: {} of {} chunks moving (cause {})", e.name(), moving, n, int(cause));
    DestructionEvent ev{DestructionEvent::Type::Fractured, e, cause, point, impulse, moving, n};
    emit(ev);
    if (disconnected > 0 && e.valid()) {
        DestructionEvent collapse{DestructionEvent::Type::Collapsed, e, DestructionCause::Collapse, point, glm::vec3(0.f), disconnected, n};
        emit(collapse);
    }
    enforceBudget();
    return true;
}

bool DestructionRuntime::releaseChunks(entt::entity ownerHandle, const std::vector<u32>& indices, const glm::vec3& point,
                                       const glm::vec3& impulse, DestructionCause cause, bool collapse) {
    auto ot = m_owners.find(ownerHandle);
    if (ot == m_owners.end() || !ot->second.fracture) return false;
    OwnerRecord& owner = ot->second;
    const physics::FractureResult& fr = *owner.fracture->result;
    const auto* d = m_world->valid(ownerHandle) ? m_world->registry().try_get<DestructibleComponent>(ownerHandle) : nullptr;
    const f32 jLen = glm::length(impulse);
    u32 released = 0;
    std::vector<entt::entity> list;
    for (u32 idx : indices) {
        if (idx >= owner.chunks.size()) continue;
        const entt::entity c = owner.chunks[idx];
        auto it = c != entt::null ? m_chunks.find(c) : m_chunks.end();
        if (it == m_chunks.end() || !it->second.frozen || !m_world->valid(c)) continue;
        if (std::find(list.begin(), list.end(), c) == list.end()) list.push_back(c);
    }
    for (entt::entity c : list) {
        ChunkRecord& rec = m_chunks[c];
        Entity ce = m_world->wrap(c);
        glm::vec3 v{0.f};
        if (jLen > 0.f && d) {
            const glm::vec3 center = ce.worldPosition();
            const glm::vec3 radial = glm::length2(center - point) > 1e-8f ? glm::normalize(center - point) : impulse / jLen;
            const f32 density = d->density > 0.f ? d->density : ce.get<ColliderComponent>().density;
            const f32 mass = std::max(density * rec.volume, 1e-3f);
            v = glm::normalize(impulse / jLen + radial * d->spread) * (jLen * d->impactImpulseScale / f32(list.size())) / mass;
        }
        rec.frozen = false;
        rec.age = 0.f;
        rec.order = ++m_spawnCounter;
        ce.get<DestructionChunkComponent>().frozen = false;
        ce.patch<RigidBodyComponent>([&](RigidBodyComponent& rb) {
            rb.motionType = physics::MotionType::Dynamic;
            rb.initialLinearVelocity = v;
            rb.initialAngularVelocity = glm::vec3(0.f);
        });
        ++released;
    }
    (void)fr;
    if (released > 0 && m_world->valid(ownerHandle)) {
        DestructionEvent ev{collapse ? DestructionEvent::Type::Collapsed : DestructionEvent::Type::ChunksReleased,
                            m_world->wrap(ownerHandle), cause, point, impulse, released, u32(owner.chunks.size())};
        emit(ev);
    }
    if (!collapse) collapseDisconnected(ownerHandle, point);
    enforceBudget();
    return released > 0;
}

u32 DestructionRuntime::collapseDisconnected(entt::entity ownerHandle, const glm::vec3& point) {
    auto ot = m_owners.find(ownerHandle);
    if (ot == m_owners.end() || !ot->second.fracture) return 0;
    const OwnerRecord& owner = ot->second;
    const u32 n = u32(owner.chunks.size());
    std::vector<bool> frozen(n, false), anchored(n, false), held(n, false);
    for (u32 i = 0; i < n; ++i) {
        auto it = owner.chunks[i] != entt::null ? m_chunks.find(owner.chunks[i]) : m_chunks.end();
        if (it == m_chunks.end() || !it->second.frozen) continue;
        frozen[i] = true;
        anchored[i] = it->second.anchored;
    }
    const auto adj = owner.fracture->result->adjacency();
    std::vector<u32> stack;
    for (u32 i = 0; i < n; ++i) {
        if (frozen[i] && anchored[i]) {
            held[i] = true;
            stack.push_back(i);
        }
    }
    while (!stack.empty()) {
        const u32 i = stack.back();
        stack.pop_back();
        for (u32 j : adj[i]) {
            if (frozen[j] && !held[j]) {
                held[j] = true;
                stack.push_back(j);
            }
        }
    }
    std::vector<u32> fall;
    for (u32 i = 0; i < n; ++i) {
        if (frozen[i] && !held[i]) fall.push_back(i);
    }
    if (fall.empty()) return 0;
    releaseChunks(ownerHandle, fall, point, glm::vec3(0.f), DestructionCause::Collapse, true);
    return u32(fall.size());
}

void DestructionRuntime::emit(const DestructionEvent& ev) {
    m_supportCheck = 2; // re-evaluate Static anchors once the new chunk bodies exist
    onDestruction.emit(ev);
    if (m_bus) m_bus->publish(ev);
    if (!m_services || !ev.entity.valid()) return;
    if (auto* scripts = m_services->tryGet<ScriptRuntime>()) {
        const char* name = ev.type == DestructionEvent::Type::Fractured  ? "broken"
                           : ev.type == DestructionEvent::Type::Collapsed ? "collapsed"
                                                                          : "chunksReleased";
        scripts->sendEvent(ev.entity, name, script::ScriptValue(f64(ev.chunks)));
    }
    if (ev.type == DestructionEvent::Type::Fractured) {
        const auto* d = ev.entity.tryGet<DestructibleComponent>();
        if (d && !d->breakSound.empty()) {
            if (auto* audio = m_services->tryGet<AudioRuntime>()) audio->playOneShot(d->breakSound, &ev.point);
        }
    }
}

// ---- per fixed step ------------------------------------------------------------------------------------------------

void DestructionRuntime::processContacts() {
    if (m_contacts.empty()) return;
    std::vector<PendingContact> contacts = std::exchange(m_contacts, {});
    entt::registry& r = m_world->registry();
    for (const PendingContact& c : contacts) {
        for (int side = 0; side < 2; ++side) {
            const entt::entity self = side == 0 ? c.a : c.b;
            const entt::entity other = side == 0 ? c.b : c.a;
            if (self == entt::null || !r.valid(self)) continue;
            // Normal points from a to b: b pushes a along -normal.
            const glm::vec3 j = (side == 0 ? -c.normal : c.normal) * c.impulse;
            if (auto* d = r.try_get<DestructibleComponent>(self); d && !d->broken) {
                if (d->impulseThreshold > 0.f && c.impulse >= d->impulseThreshold) {
                    fracture(m_world->wrap(self), c.point, j, DestructionCause::Impact);
                } else if (d->impulseDamageScale > 0.f) {
                    applyDamage(m_world->wrap(self), c.impulse * d->impulseDamageScale, c.point, j, DestructionCause::Impact);
                }
                continue;
            }
            auto it = m_chunks.find(self);
            if (it == m_chunks.end() || !it->second.frozen) continue;
            const entt::entity owner = it->second.owner;
            if (other != entt::null) {
                auto ot = m_chunks.find(other);
                if (ot != m_chunks.end() && ot->second.owner == owner) continue; // siblings never release each other
            }
            const auto* d = r.valid(owner) ? r.try_get<DestructibleComponent>(owner) : nullptr;
            if (!d) continue;
            const f32 threshold = d->chunkImpulseThreshold > 0.f ? d->chunkImpulseThreshold : d->impulseThreshold;
            if (threshold > 0.f && c.impulse >= threshold) {
                applyDamage(m_world->wrap(self), std::max(d->damageThreshold, 1.f), c.point, j, DestructionCause::Impact);
            }
        }
    }
}

void DestructionRuntime::processReplicated() {
    // A `broken` flag set by replication / a loaded save, or health brought to zero by scripts: break now.
    std::vector<entt::entity> pending;
    for (auto [e, d] : m_world->registry().view<DestructibleComponent>().each()) {
        auto it = m_owners.find(e);
        const bool fractured = it != m_owners.end() && it->second.fractured;
        if (fractured) continue;
        if (d.broken || d.health <= 0.f) pending.push_back(e);
    }
    std::sort(pending.begin(), pending.end());
    for (auto e : pending) {
        Entity en = m_world->wrap(e);
        const bool replicated = en.get<DestructibleComponent>().broken;
        en.get<DestructibleComponent>().broken = false;
        if (!fracture(en, en.worldPosition(), glm::vec3(0.f), replicated ? DestructionCause::Replicated : DestructionCause::Damage)) {
            en.get<DestructibleComponent>().broken = replicated;
        }
    }
}

void DestructionRuntime::startFade(ChunkRecord& rec, f32 duration) {
    if (rec.fading) return;
    Entity e = m_world->wrap(rec.entity);
    rec.fading = true;
    rec.fadeTime = 0.f;
    rec.fadeDuration = std::max(duration, 1e-3f);
    rec.fadeScale = e.localTransform().scale;
    // The body goes away: the chunk shrinks in place.
    if (e.has<RigidBodyComponent>()) e.remove<RigidBodyComponent>();
    if (e.has<ColliderComponent>()) e.remove<ColliderComponent>();
}

void DestructionRuntime::updateDebris(f32 dt) {
    const f32 lifetimeScale = std::max(cvLifetimeScale.get(), 0.f);
    std::vector<entt::entity> done;
    for (auto& [h, rec] : m_chunks) {
        if (!m_world->valid(h) || m_world->registry().all_of<PendingDestroyTag>(h)) {
            done.push_back(h);
            continue;
        }
        if (rec.frozen) continue;
        Entity e = m_world->wrap(h);
        if (rec.fading) {
            rec.fadeTime += dt;
            const f32 t = std::clamp(rec.fadeTime / rec.fadeDuration, 0.f, 1.f);
            e.setScale(rec.fadeScale * std::max(1.f - t, 1e-3f));
            if (t >= 1.f) {
                e.destroy();
                done.push_back(h);
            }
            continue;
        }
        rec.age += dt;
        const f32 lifetime = rec.lifetime * lifetimeScale;
        if (lifetime <= 0.f || rec.age < lifetime) continue;
        bool sleeping = true;
        if (m_physics) {
            const physics::BodyHandle b = m_physics->bodyOf(e);
            sleeping = !b.valid() || !m_physics->physicsWorld().isActive(b);
        }
        if (sleeping || rec.age >= lifetime * 2.f) startFade(rec, rec.fadeDuration);
    }
    for (auto h : done) m_chunks.erase(h);
}

void DestructionRuntime::enforceBudget() {
    std::vector<ChunkRecord*> active;
    for (auto& [h, rec] : m_chunks) {
        if (!rec.frozen && !rec.fading && m_world->valid(h)) active.push_back(&rec);
    }
    std::sort(active.begin(), active.end(), [](const ChunkRecord* a, const ChunkRecord* b) { return a->order < b->order; });
    // Per object.
    std::unordered_map<entt::entity, u32> perOwner;
    for (ChunkRecord* rec : active) ++perOwner[rec->owner];
    for (ChunkRecord* rec : active) {
        if (rec->ownerLimit == 0) continue;
        u32& count = perOwner[rec->owner];
        if (count > rec->ownerLimit) {
            startFade(*rec, std::min(rec->fadeDuration, kBudgetFade));
            --count;
        }
    }
    // Global.
    const u32 limit = budget();
    u32 count = 0;
    for (ChunkRecord* rec : active) count += rec->fading ? 0u : 1u;
    for (ChunkRecord* rec : active) {
        if (limit == 0 || count <= limit) break;
        if (rec->fading) continue;
        startFade(*rec, std::min(rec->fadeDuration, kBudgetFade));
        --count;
    }
}

void DestructionRuntime::fixedUpdate(f32 dt) {
    if (!m_playing || !m_world) return;
    OX_PROFILE_ZONE_N("DestructionRuntime::fixedUpdate");
    processContacts();
    processReplicated();
    if (m_supportCheck > 0 && --m_supportCheck == 0) checkSupports();
    updateDebris(dt);
    enforceBudget();
}

// ---- static supports ---------------------------------------------------------------------------------------------

std::vector<std::pair<entt::entity, glm::vec3>> DestructionRuntime::staticSupports(const glm::vec3& mn, const glm::vec3& mx,
                                                                                   entt::entity self, f32 tolerance) const {
    std::vector<std::pair<entt::entity, glm::vec3>> out;
    if (!m_physics || !m_world) return out;
    const physics::PhysicsWorld& pw = m_physics->physicsWorld();
    const glm::vec3 t(std::max(tolerance, 1e-3f));
    for (physics::BodyHandle b : pw.overlapAabb({mn - t, mx + t})) {
        if (pw.getMotionType(b) != physics::MotionType::Static) continue;
        const Entity other = m_physics->entityOf(b);
        if (!other.valid()) continue;
        entt::entity id = other.handle();
        if (auto it = m_chunks.find(id); it != m_chunks.end()) {
            if (!it->second.frozen) continue;
            id = it->second.owner; // a frozen chunk supports on behalf of its (broken) object
        }
        if (id == self) continue;
        bool known = false;
        for (const auto& [eid, p] : out) known |= eid == id;
        if (!known) out.emplace_back(id, glm::clamp(other.worldPosition(), mn, mx));
    }
    return out;
}

void DestructionRuntime::recordSupports(Entity e, OwnerRecord& owner) {
    owner.supportsRecorded = true;
    owner.supports.clear();
    if (!owner.fracture) return;
    const auto& d = e.get<DestructibleComponent>();
    glm::vec3 mn, mx;
    worldBounds(e.worldTransform(), owner.fracture->result->boundsMin, owner.fracture->result->boundsMax, mn, mx);
    owner.supports = staticSupports(mn, mx, e.handle(), d.anchorTolerance);
}

void DestructionRuntime::checkSupports() {
    std::vector<entt::entity> owners;
    for (auto [e, d] : m_world->registry().view<DestructibleComponent>().each()) {
        if (d.structural && d.anchor == DestructionAnchor::Static) owners.push_back(e);
    }
    std::sort(owners.begin(), owners.end());
    for (entt::entity h : owners) {
        Entity e = m_world->wrap(h);
        auto it = m_owners.find(h);
        if (it == m_owners.end() || !it->second.fracture) continue;
        OwnerRecord& owner = it->second;
        const auto& d = e.get<DestructibleComponent>();
        if (!owner.fractured) {
            // Intact: a support it rested on is gone -> it breaks there (structurally: what still rests on the other
            // supports stays, the rest collapses through the link graph).
            if (!owner.supportsRecorded || owner.supports.empty() || d.broken) continue;
            glm::vec3 mn, mx;
            worldBounds(e.worldTransform(), owner.fracture->result->boundsMin, owner.fracture->result->boundsMax, mn, mx);
            const auto now = staticSupports(mn, mx, h, d.anchorTolerance);
            for (const auto& [support, point] : owner.supports) {
                bool alive = false;
                for (const auto& [id, p] : now) alive |= id == support;
                if (alive) continue;
                fracture(e, point, glm::vec3(0.f), DestructionCause::Collapse);
                break;
            }
            continue;
        }
        // Broken: frozen chunks anchored on static bodies re-check them.
        bool changed = false;
        const Transform wt = e.worldTransform();
        for (entt::entity c : owner.chunks) {
            auto ct = c != entt::null ? m_chunks.find(c) : m_chunks.end();
            if (ct == m_chunks.end() || !ct->second.frozen || !ct->second.anchored) continue;
            const physics::FractureChunk& fc = owner.fracture->result->chunks[ct->second.index];
            glm::vec3 mn, mx;
            worldBounds(wt, fc.boundsMin, fc.boundsMax, mn, mx);
            if (!staticSupports(mn, mx, h, d.anchorTolerance).empty()) continue;
            ct->second.anchored = false;
            if (m_world->valid(c)) m_world->wrap(c).get<DestructionChunkComponent>().anchored = false;
            changed = true;
        }
        if (changed) collapseDisconnected(h, e.worldPosition());
    }
}

void DestructionRuntime::drawDebug(DebugDraw& draw) const {
    if (!debugDraw || !m_world || !m_playing) return;
    for (const auto& [ownerHandle, owner] : m_owners) {
        if (!owner.fracture || owner.chunks.empty()) continue;
        auto pos = [&](u32 i, glm::vec3& out) {
            const entt::entity c = owner.chunks[i];
            auto it = c != entt::null ? m_chunks.find(c) : m_chunks.end();
            if (it == m_chunks.end() || !it->second.frozen || !m_world->valid(c)) return false;
            out = m_world->wrap(c).worldPosition();
            return true;
        };
        for (const physics::FractureLink& l : owner.fracture->result->links) {
            glm::vec3 a, b;
            if (l.a < owner.chunks.size() && l.b < owner.chunks.size() && pos(l.a, a) && pos(l.b, b)) {
                draw.line(a, b, debug_color::kYellow);
            }
        }
        for (u32 i = 0; i < owner.chunks.size(); ++i) {
            glm::vec3 p;
            if (!pos(i, p)) continue;
            const bool anchored = m_chunks.at(owner.chunks[i]).anchored;
            draw.sphere(p, 0.04f, anchored ? DebugColor(debug_color::kGreen) : DebugColor(debug_color::kOrange));
        }
    }
}

} // namespace ox::gameplay
