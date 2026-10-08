#include "world_runtime_impl.hpp"

#include <oxwald/core/profile.hpp>
#include <oxwald/gameplay/common.hpp>

#include <algorithm>

namespace ox::gameplay {

namespace {

class JobSystemExecutor final : public world::IChunkExecutor {
public:
    explicit JobSystemExecutor(JobSystem& jobs) : m_jobs(jobs) {}
    void submit(std::function<void()> job) override { m_jobs.submit(std::move(job)); }

private:
    JobSystem& m_jobs;
};

std::shared_ptr<world::IChunkExecutor> makeExecutor(WorldSystemsConfig::Executor kind, Services& services) {
    using Executor = WorldSystemsConfig::Executor;
    if (kind == Executor::ThreadPool) return std::make_shared<world::ThreadPoolExecutor>(2);
    if (kind == Executor::Auto || kind == Executor::JobSystem) {
        // A job system without workers would deadlock the streamer's flush/destructor (waiting from the main thread
        // does not help with work submitted from it).
        if (auto* jobs = services.tryGet<JobSystem>(); jobs && jobs->threadCount() > 1) {
            return std::make_shared<JobSystemExecutor>(*jobs);
        }
    }
    return std::make_shared<world::InlineExecutor>();
}

} // namespace

std::string formatChunkPattern(std::string_view pattern, world::ChunkCoord c) {
    std::string out;
    out.reserve(pattern.size() + 8);
    for (usize i = 0; i < pattern.size(); ++i) {
        if (pattern.substr(i, 3) == "{x}") {
            out += std::to_string(c.x);
            i += 2;
        } else if (pattern.substr(i, 3) == "{z}") {
            out += std::to_string(c.z);
            i += 2;
        } else {
            out += pattern[i];
        }
    }
    return out;
}

// ---- Impl helpers ------------------------------------------------------------------------------------------

physics::BodyHandle WorldRuntime::Impl::createStaticBody(const physics::ShapeDesc& shape, glm::vec3 position, glm::quat rotation,
                                                         u64 userData, f32 friction, f32 restitution, bool cacheShape) {
    if (!physicsRt) return {};
    physics::PhysicsWorld& pw = physicsRt->physicsWorld();
    std::string error;
    physics::BodyDesc d;
    d.shape = cacheShape ? pw.shapeCache().getOrCreate(shape, &error) : physics::createShape(shape, &error);
    if (!d.shape) {
        OX_LOG_WARN("gameplay.world", "collider shape creation failed: {}", error);
        return {};
    }
    d.position = position;
    d.rotation = rotation;
    d.motionType = physics::MotionType::Static;
    d.friction = friction;
    d.restitution = restitution;
    d.userData = userData;
    return pw.createBody(d);
}

void WorldRuntime::Impl::destroyBody(physics::BodyHandle& body) {
    if (body && physicsRt && physicsRt->physicsWorld().isValid(body)) physicsRt->physicsWorld().destroyBody(body);
    body = {};
}

Entity WorldRuntime::Impl::primaryCamera() const {
    if (!world) return {};
    entt::entity primary{entt::null}, any{entt::null};
    for (auto [e, cam] : world->registry().view<CameraComponent>().each()) {
        if (world->registry().all_of<PendingDestroyTag>(e) || !world->wrap(e).activeInHierarchy()) continue;
        auto lower = [](entt::entity a, entt::entity b) { return a == entt::null || entt::to_integral(b) < entt::to_integral(a); };
        if (cam.primary && lower(primary, e)) primary = e;
        if (lower(any, e)) any = e;
    }
    const entt::entity pick = primary != entt::null ? primary : any;
    return pick == entt::null ? Entity{} : world->wrap(pick);
}

std::vector<world::StreamingViewer> WorldRuntime::Impl::viewers(bool includeCamera) const {
    std::vector<world::StreamingViewer> out;
    if (!world) return out;
    std::vector<entt::entity> sources;
    for (auto [e, src] : world->registry().view<StreamingSourceComponent>().each()) {
        if (src.enabled && !world->registry().all_of<PendingDestroyTag>(e)) sources.push_back(e);
    }
    std::sort(sources.begin(), sources.end());
    for (auto e : sources) {
        const Entity ent = world->wrap(e);
        if (!ent.activeInHierarchy()) continue;
        world::StreamingViewer v;
        const Transform t = ent.worldTransform();
        v.position = t.position;
        v.forward = glm::normalize(t.rotation) * glm::vec3(0.f, 0.f, -1.f);
        v.radiusScale = world->registry().get<StreamingSourceComponent>(e).radiusScale;
        v.id = entt::to_integral(e);
        out.push_back(v);
    }
    if (includeCamera) {
        if (Entity cam = primaryCamera(); cam.valid()) {
            const Transform t = cam.worldTransform();
            world::StreamingViewer v;
            v.position = t.position;
            v.forward = glm::normalize(t.rotation) * glm::vec3(0.f, 0.f, -1.f);
            v.id = 0x80000000u | entt::to_integral(cam.handle());
            out.push_back(v);
        }
    }
    return out;
}

void WorldRuntime::Impl::clearAll() {
    for (auto& [e, st] : streaming) stopStreaming(st);
    streaming.clear();
    for (auto& [e, st] : terrains) destroyTerrainBodies(st);
    terrains.clear();
    for (auto& [e, st] : vegetation) clearVegetation(st);
    vegetation.clear();
    timeOfDays.clear();
    waters.clear();
    winds.clear();
    buoyancy.clear();
    globalWind = entt::null;
}

void WorldRuntime::Impl::onTerrainChanged(entt::registry&, entt::entity e) { terrains[e].dirty = true; }

void WorldRuntime::Impl::onTerrainDestroyed(entt::registry&, entt::entity e) {
    auto it = terrains.find(e);
    if (it == terrains.end()) return;
    destroyTerrainBodies(it->second);
    terrains.erase(it);
}

void WorldRuntime::Impl::onVegetationChanged(entt::registry&, entt::entity e) { vegetation[e].dirty = true; }

void WorldRuntime::Impl::onVegetationDestroyed(entt::registry&, entt::entity e) {
    auto it = vegetation.find(e);
    if (it == vegetation.end()) return;
    clearVegetation(it->second);
    vegetation.erase(it);
}

void WorldRuntime::Impl::onTimeOfDayChanged(entt::registry&, entt::entity e) { timeOfDays[e].dirty = true; }

void WorldRuntime::Impl::onStreamingDestroyed(entt::registry&, entt::entity e) {
    auto it = streaming.find(e);
    if (it == streaming.end()) return;
    stopStreaming(it->second);
    streaming.erase(it);
}

// ---- WorldRuntime ---------------------------------------------------------------------------------------------

WorldRuntime::WorldRuntime(const WorldSystemsConfig& config) : m(std::make_unique<Impl>(*this, config)) {}

WorldRuntime::~WorldRuntime() { detach(); }

World* WorldRuntime::world() const { return m->world; }
bool WorldRuntime::simulating() const { return m->playing; }
f64 WorldRuntime::gameTime() const { return m->time; }
const WorldSystemsConfig& WorldRuntime::config() const { return m->config; }

void WorldRuntime::attach(World& world, Services& services) {
    detach();
    m->world = &world;
    m->services = &services;
    m->physicsRt = services.tryGet<PhysicsRuntime>();
    m->heightmaps = services.tryGet<IHeightmapProvider>();
    m->prefabs = services.tryGet<IPrefabProvider>();
    m->bus = services.tryGet<EventBus>();
    m->vfs = services.tryGet<Vfs>();
    m->executor = makeExecutor(m->config.streamingExecutor, services);
    m->playing = false;
    m->time = 0.0;
    if (auto* events = services.tryGet<GameplayAssetEvents>()) {
        m->assetConnection = events->changed.connect([impl = m.get()](const GameplayAssetChange& c) { impl->onAssetChanged(c); });
    }

    entt::registry& r = world.registry();
    auto add = [&](entt::connection c) { m->connections.emplace_back(c); };
    Impl& i = *m;
    add(r.on_construct<TerrainComponent>().connect<&Impl::onTerrainChanged>(i));
    add(r.on_update<TerrainComponent>().connect<&Impl::onTerrainChanged>(i));
    add(r.on_destroy<TerrainComponent>().connect<&Impl::onTerrainDestroyed>(i));
    add(r.on_construct<VegetationComponent>().connect<&Impl::onVegetationChanged>(i));
    add(r.on_update<VegetationComponent>().connect<&Impl::onVegetationChanged>(i));
    add(r.on_destroy<VegetationComponent>().connect<&Impl::onVegetationDestroyed>(i));
    add(r.on_construct<TimeOfDayComponent>().connect<&Impl::onTimeOfDayChanged>(i));
    add(r.on_update<TimeOfDayComponent>().connect<&Impl::onTimeOfDayChanged>(i));
    add(r.on_destroy<TimeOfDayComponent>().connect<&Impl::onTimeOfDayDestroyed>(i));
    add(r.on_construct<WaterComponent>().connect<&Impl::onWaterChanged>(i));
    add(r.on_update<WaterComponent>().connect<&Impl::onWaterChanged>(i));
    add(r.on_destroy<WaterComponent>().connect<&Impl::onWaterDestroyed>(i));
    add(r.on_construct<WindComponent>().connect<&Impl::onWindChanged>(i));
    add(r.on_update<WindComponent>().connect<&Impl::onWindChanged>(i));
    add(r.on_destroy<WindComponent>().connect<&Impl::onWindDestroyed>(i));
    add(r.on_construct<BuoyancyComponent>().connect<&Impl::onBuoyancyChanged>(i));
    add(r.on_update<BuoyancyComponent>().connect<&Impl::onBuoyancyChanged>(i));
    add(r.on_destroy<BuoyancyComponent>().connect<&Impl::onBuoyancyDestroyed>(i));
    add(r.on_construct<WorldStreamingComponent>().connect<&Impl::onStreamingChanged>(i));
    add(r.on_update<WorldStreamingComponent>().connect<&Impl::onStreamingChanged>(i));
    add(r.on_destroy<WorldStreamingComponent>().connect<&Impl::onStreamingDestroyed>(i));
}

void WorldRuntime::detach() {
    if (!m->world) return;
    m->clearAll();
    m->connections.clear();
    m->assetConnection.disconnect();
    m->executor.reset();
    m->world = nullptr;
    m->services = nullptr;
    m->physicsRt = nullptr;
    m->heightmaps = nullptr;
    m->prefabs = nullptr;
    m->bus = nullptr;
    m->vfs = nullptr;
    m->playing = false;
}

void WorldRuntime::syncPlayState(bool playing) {
    if (!m->world || playing == m->playing) return;
    if (playing) {
        m->playing = true;
        m->time = 0.0;
        // Bodies / streamers are created lazily by the update systems this frame.
        for (auto& [e, st] : m->terrains) st.bodiesBuilt = false;
        return;
    }
    for (auto& [e, st] : m->streaming) {
        m->stopStreaming(st);
        st.dirty = true;
    }
    for (auto& [e, st] : m->terrains) m->destroyTerrainBodies(st);
    for (auto& [e, st] : m->vegetation) {
        for (auto& [c, chunk] : st.chunks) {
            for (auto& b : chunk.bodies) m->destroyBody(b);
            chunk.bodies.clear();
            chunk.bodiesBuilt = false;
        }
    }
    for (auto& [e, st] : m->buoyancy) st.submergedFraction = 0.f;
    m->playing = false;
}

void WorldRuntime::advanceTime(f32 dt) { m->time += static_cast<f64>(dt); }

// ---- terrain queries ---------------------------------------------------------------------------------------

const TerrainState* WorldRuntime::Impl::terrainState(Entity e) const {
    if (!e.valid() || e.world() != world) return nullptr;
    auto it = terrains.find(e.handle());
    return it != terrains.end() && it->second.hf ? &it->second : nullptr;
}

Entity WorldRuntime::terrainAt(glm::vec2 xz) const {
    if (!m->world) return {};
    entt::entity best{entt::null};
    for (const auto& [e, st] : m->terrains) {
        if (!st.hf || !st.hf->containsWorld(xz) || !m->world->valid(e)) continue;
        if (best == entt::null || entt::to_integral(e) < entt::to_integral(best)) best = e;
    }
    return best == entt::null ? Entity{} : m->world->wrap(best);
}

std::optional<f32> WorldRuntime::terrainHeight(glm::vec2 xz) const {
    const TerrainState* st = m->terrainState(terrainAt(xz));
    if (!st) return std::nullopt;
    if (st->hf->hasHoles()) {
        const glm::vec2 s = glm::round(st->hf->worldToSample(xz));
        if (st->hf->isHole(u32(s.x), u32(s.y))) return std::nullopt;
    }
    return st->hf->sampleHeight(xz);
}

std::optional<glm::vec3> WorldRuntime::terrainNormal(glm::vec2 xz) const {
    const TerrainState* st = m->terrainState(terrainAt(xz));
    if (!st) return std::nullopt;
    return st->hf->sampleNormal(xz);
}

std::shared_ptr<const world::Heightfield> WorldRuntime::heightfield(Entity e) const {
    const TerrainState* st = m->terrainState(e);
    return st ? st->hf : nullptr;
}

std::shared_ptr<const world::SplatMap> WorldRuntime::splatMap(Entity e) const {
    const TerrainState* st = m->terrainState(e);
    return st ? st->splat : nullptr;
}

std::shared_ptr<const world::TerrainQuadtree> WorldRuntime::quadtree(Entity e) const {
    const TerrainState* st = m->terrainState(e);
    return st ? st->quadtree : nullptr;
}

u64 WorldRuntime::terrainVersion(Entity e) const {
    const TerrainState* st = m->terrainState(e);
    return st ? st->version : 0;
}

u32 WorldRuntime::terrainBodyCount(Entity e) const {
    const TerrainState* st = m->terrainState(e);
    if (!st) return 0;
    return static_cast<u32>(std::count_if(st->bodies.begin(), st->bodies.end(), [](const auto& kv) { return kv.second.valid(); }));
}

world::IRect WorldRuntime::applyBrush(Entity e, glm::vec2 centerXZ, const world::BrushSettings& brush, f32 dt) {
    if (!e.valid() || e.world() != m->world) return {};
    auto it = m->terrains.find(e.handle());
    if (it == m->terrains.end() || !it->second.hf) return {};
    TerrainState& st = it->second;
    const world::IRect dirty = world::applyBrush(*st.hf, centerXZ, brush, dt);
    if (dirty.empty()) return dirty;
    if (st.quadtree) st.quadtree->updateBounds(*st.hf, dirty);
    st.dirtyRect = st.dirtyRect.empty() ? dirty : st.dirtyRect.merged(dirty);
    ++st.version;
    if (st.bodiesBuilt) m->rebuildTerrainTiles(e, st, dirty);
    if (auto* c = e.tryGet<TerrainComponent>()) {
        c->minHeight = st.hf->minHeight();
        c->maxHeight = st.hf->maxHeight();
    }
    return dirty;
}

world::IRect WorldRuntime::paintSplat(Entity e, glm::vec2 centerXZ, u32 layer, const world::BrushSettings& brush, f32 dt) {
    if (!e.valid() || e.world() != m->world) return {};
    auto it = m->terrains.find(e.handle());
    if (it == m->terrains.end() || !it->second.splat) return {};
    TerrainState& st = it->second;
    const world::IRect dirty = world::paintSplat(*st.splat, centerXZ, layer, brush, dt);
    if (dirty.empty()) return dirty;
    st.splatDirtyRect = st.splatDirtyRect.empty() ? dirty : st.splatDirtyRect.merged(dirty);
    ++st.splatVersion;
    ++st.version; // vegetation rules may depend on splat weights
    return dirty;
}

void WorldRuntime::setExternalTerrain(Entity e, ExternalTerrainData data) {
    if (!e.valid() || e.world() != m->world) return;
    TerrainState& st = m->terrains[e.handle()];
    st.external = std::move(data);
    st.hasExternal = true;
    st.dirty = true;
}

void WorldRuntime::rebuildTerrain(Entity e) {
    if (!e.valid() || e.world() != m->world) return;
    if (auto it = m->terrains.find(e.handle()); it != m->terrains.end()) it->second.dirty = true;
}

// ---- vegetation queries ------------------------------------------------------------------------------------

u32 WorldRuntime::vegetationInstanceCount(Entity e) const {
    if (!e.valid() || e.world() != m->world) return 0;
    auto it = m->vegetation.find(e.handle());
    if (it == m->vegetation.end()) return 0;
    usize n = 0;
    for (const auto& [c, chunk] : it->second.chunks) n += chunk.chunk.instances.size();
    return static_cast<u32>(n);
}

u32 WorldRuntime::vegetationBodyCount(Entity e) const {
    if (!e.valid() || e.world() != m->world) return 0;
    auto it = m->vegetation.find(e.handle());
    if (it == m->vegetation.end()) return 0;
    usize n = 0;
    for (const auto& [c, chunk] : it->second.chunks) n += chunk.bodies.size();
    return static_cast<u32>(n);
}

// ---- buoyancy ------------------------------------------------------------------------------------------------

f32 WorldRuntime::submergedFraction(Entity e) const {
    if (!e.valid() || e.world() != m->world) return 0.f;
    auto it = m->buoyancy.find(e.handle());
    return it == m->buoyancy.end() ? 0.f : it->second.submergedFraction;
}

void WorldRuntime::fixedBuoyancy(f32 /*dt*/) {
    OX_PROFILE_ZONE();
    if (!m->world || !m->physicsEnabled()) return;
    physics::PhysicsWorld& pw = m->physicsRt->physicsWorld();
    const f32 g = std::max(-pw.gravity().y, 0.f);
    const f32 t = static_cast<f32>(m->time);
    entt::registry& r = m->world->registry();
    for (auto [e, bc] : r.view<BuoyancyComponent>().each()) {
        const Entity ent = m->world->wrap(e);
        const physics::BodyHandle body = m->physicsRt->bodyOf(ent);
        BuoyancyState& st = m->buoyancy[e];
        if (!body || pw.getMotionType(body) != physics::MotionType::Dynamic) {
            st.submergedFraction = bc.submergedFraction = 0.f;
            continue;
        }
        if (st.dirty) {
            st.settings = bc.points.empty() ? world::BuoyancySettings::fromBox(bc.halfExtents, std::max(1u, bc.subdivisions))
                                            : world::BuoyancySettings{};
            if (!bc.points.empty()) st.settings.points = bc.points;
            st.settings.linearDrag = bc.linearDrag;
            st.settings.angularDrag = bc.angularDrag;
            st.dirty = false;
        }
        const glm::vec3 com = pw.getCenterOfMassPosition(body);
        const WaterState* water = nullptr;
        (void)m->waterHeightAt({com.x, com.z}, t, &water);
        st.settings.fluidDensity = bc.fluidDensity > 0.f ? bc.fluidDensity : (water ? water->density : 1000.f);
        st.settings.gravity = g;
        const glm::vec3 current = water ? water->current : glm::vec3(0.f);
        const world::BuoyancyResult res = world::computeBuoyancy(
            st.settings, com, pw.getRotation(body), pw.getLinearVelocity(body), pw.getAngularVelocity(body),
            [&](glm::vec2 xz) {
                const auto h = m->waterHeightAt(xz, t);
                return h ? *h : -1e30f;
            },
            current);
        if (res.submergedVolume > 0.f) {
            pw.addForce(body, res.force);
            pw.addTorque(body, res.torque);
        }
        st.submergedFraction = bc.submergedFraction = res.submergedFraction;
    }
}

// ---- water / wind / time-of-day queries ------------------------------------------------------------------------

std::optional<f32> WorldRuntime::waterHeight(glm::vec2 xz) const { return m->waterHeightAt(xz, static_cast<f32>(m->time)); }
std::optional<f32> WorldRuntime::waterHeight(glm::vec2 xz, f32 time) const { return m->waterHeightAt(xz, time); }

glm::vec3 WorldRuntime::windAt(glm::vec3 p) const {
    auto it = m->winds.find(m->globalWind);
    if (it == m->winds.end()) return glm::vec3(0.f);
    return it->second.field.sample(p, static_cast<f32>(m->time));
}

const world::WeatherState* WorldRuntime::weather() const {
    if (!m->world || m->globalWind == entt::null || !m->world->valid(m->globalWind)) return nullptr;
    const auto* c = m->world->registry().try_get<WindComponent>(m->globalWind);
    auto it = m->winds.find(m->globalWind);
    if (!c || !c->weatherEnabled || it == m->winds.end()) return nullptr;
    return &it->second.weather.state();
}

bool WorldRuntime::setWeather(const world::WeatherPreset& preset, f32 seconds) {
    if (!m->world || m->globalWind == entt::null || !m->world->valid(m->globalWind)) return false;
    auto* c = m->world->registry().try_get<WindComponent>(m->globalWind);
    if (!c) return false;
    WindState& st = m->winds[m->globalWind];
    if (!c->weatherEnabled || !st.weatherInitialized) {
        st.weather = world::WeatherController(c->weather);
        st.weatherInitialized = true;
    }
    c->weatherEnabled = true;
    c->weather = preset;
    st.target = preset;
    st.weather.setTarget(preset, std::max(seconds, 0.f));
    return true;
}

Entity WorldRuntime::timeOfDayEntity() const {
    if (!m->world) return {};
    const entt::entity e = m->firstWith<TimeOfDayComponent>();
    return e == entt::null ? Entity{} : m->world->wrap(e);
}

std::optional<f64> WorldRuntime::timeOfDay() const {
    const Entity e = timeOfDayEntity();
    if (!e.valid()) return std::nullopt;
    auto it = m->timeOfDays.find(e.handle());
    if (it != m->timeOfDays.end() && it->second.tod && !it->second.dirty) return it->second.tod->localHours();
    return e.get<TimeOfDayComponent>().localHours;
}

bool WorldRuntime::setTimeOfDay(f64 hours) {
    const Entity e = timeOfDayEntity();
    if (!e.valid()) return false;
    auto& c = e.get<TimeOfDayComponent>();
    c.localHours = hours;
    TimeOfDayState& st = m->timeOfDays[e.handle()];
    if (st.tod && !st.dirty) {
        st.tod->setLocalTime(hours);
        c.localHours = st.tod->localHours();
        c.year = st.tod->settings().year;
        c.month = st.tod->settings().month;
        c.day = st.tod->settings().day;
    } else {
        st.dirty = true;
    }
    return true;
}

const world::SkyState* WorldRuntime::skyState() const {
    const Entity e = timeOfDayEntity();
    if (!e.valid()) return nullptr;
    auto it = m->timeOfDays.find(e.handle());
    return it != m->timeOfDays.end() && it->second.tod ? &it->second.tod->state() : nullptr;
}

// ---- streaming queries -------------------------------------------------------------------------------------

bool WorldRuntime::isAreaReady(glm::vec3 p, f32 radius) const {
    for (const auto& [e, st] : m->streaming) {
        if (st.streamer && !st.streamer->isAreaReady(p, radius)) return false;
    }
    return true;
}

world::ChunkStreamer* WorldRuntime::streamer(Entity e) const {
    if (!e.valid() || e.world() != m->world) return nullptr;
    auto it = m->streaming.find(e.handle());
    return it == m->streaming.end() ? nullptr : it->second.streamer.get();
}

std::vector<Entity> WorldRuntime::chunkEntities(Entity e, world::ChunkCoord c) const {
    std::vector<Entity> out;
    if (!e.valid() || e.world() != m->world) return out;
    auto it = m->streaming.find(e.handle());
    if (it == m->streaming.end()) return out;
    auto ct = it->second.spawned.find(c);
    if (ct == it->second.spawned.end()) return out;
    for (auto h : ct->second) {
        if (m->world->valid(h)) out.push_back(m->world->wrap(h));
    }
    return out;
}

u32 WorldRuntime::spawnedChunkCount(Entity e) const {
    if (!e.valid() || e.world() != m->world) return 0;
    auto it = m->streaming.find(e.handle());
    return it == m->streaming.end() ? 0 : static_cast<u32>(it->second.spawned.size());
}

} // namespace ox::gameplay
