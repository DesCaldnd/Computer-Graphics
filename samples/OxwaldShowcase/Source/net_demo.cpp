#include "net_demo.hpp"

#include <oxwald/core/log.hpp>
#include <oxwald/gameplay/gameplay.hpp>
#include <oxwald/net/memory_transport.hpp>
#include <oxwald/render/mesh_primitives.hpp>
#include <oxwald/scene/components.hpp>

namespace ox::showcase {

namespace {
// Prefabs the station replicates (netType = prefab name) and the material of the client-side ghosts.
constexpr const char* kNetTypes[] = {"NetAvatar", "NetBot", "NetCrate"};
Uuid ghostMaterial() { return Uuid::fromName("showcase.material.Ghost"); }
} // namespace

struct NetDemo::Client {
    net::MemoryNetwork network{42};
    gameplay::GameplayAssetRegistry assets;
    World world;
    Services services;
    SystemScheduler scheduler;
    ~Client() { scheduler.detach(); }
};

NetDemo::NetDemo(ShowcaseModule& module) : m_module(module) {}
NetDemo::~NetDemo() { stop(); }

void NetDemo::onWorldChanged(World& world) {
    stop();
    m_world = &world;
    m_pending = bool(world.findByName("NetDemo"));
    m_waitFrames = 0;
}

void NetDemo::preUpdate(const FrameTime& time) {
    // Start a few frames after the level change, once the gameplay runtimes are attached to the new world.
    if (m_pending && time.playing && m_world && &m_module.engine().world() == m_world && ++m_waitFrames > 3) {
        m_pending = false;
        if (!start(*m_world)) stop();
    }
    if (!m_client || !m_world) return;
    m_client->scheduler.tick(m_client->world, m_client->services, time.realDt);
    mirrorGhosts(*m_world);
}

bool NetDemo::start(World& world) {
    auto* serverRt = m_module.engine().services().tryGet<gameplay::NetworkRuntime>();
    auto* prefabs = m_module.engine().services().tryGet<gameplay::IPrefabProvider>();
    if (!serverRt || !prefabs) {
        OX_LOG_WARN("showcase", "net demo: gameplay networking or the prefab provider is missing");
        return false;
    }
    m_client = std::make_unique<Client>();
    for (const char* type : kNetTypes) {
        if (auto doc = prefabs->prefab(type)) m_client->assets.addPrefab(type, *doc);
        else OX_LOG_WARN("showcase", "net demo: prefab '{}' not found", type);
    }
    gameplay::GameplayConfig cfg;
    cfg.physics = false;
    cfg.animation = false;
    cfg.splines = false;
    cfg.audio = false;
    cfg.ai = false;
    cfg.scripting = false;
    cfg.coroutines = false;
    cfg.world = false;
    cfg.prediction = false;
    cfg.createEventBus = true;
    m_client->assets.registerIn(m_client->services);
    addGameplaySystems(m_client->scheduler, m_client->services, cfg);
    m_client->scheduler.attach(m_client->world, m_client->services);
    m_client->scheduler.setPlaying(true);
    setConditions(m_latencyMs, m_lossPercent, m_jitterMs);

    net::NetServerConfig sc;
    sc.tickRate = 30.f;
    if (!serverRt->startServer(m_client->network.createTransport(), sc)) {
        OX_LOG_WARN("showcase", "net demo: server failed to start");
        return false;
    }
    auto& clientRt = m_client->services.get<gameplay::NetworkRuntime>();
    if (!clientRt.connect(m_client->network.createTransport(), "memory", serverRt->server()->port())) {
        OX_LOG_WARN("showcase", "net demo: bot client failed to connect");
        return false;
    }
    OX_LOG_INFO("showcase", "net demo: listen server on memory port {}, bot client connecting", serverRt->server()->port());
    (void)world;
    return true;
}

void NetDemo::stop() {
    if (m_client) {
        m_client->services.get<gameplay::NetworkRuntime>().shutdown();
        if (auto* serverRt = m_module.engine().services().tryGet<gameplay::NetworkRuntime>()) serverRt->shutdown();
        m_client.reset();
    }
    m_ghosts.clear();
    m_pending = false;
}

void NetDemo::setConditions(f32 latencyMs, f32 lossPercent, f32 jitterMs) {
    m_latencyMs = std::max(0.f, latencyMs);
    m_lossPercent = std::clamp(lossPercent, 0.f, 50.f);
    m_jitterMs = std::max(0.f, jitterMs);
    if (!m_client) return;
    net::LinkConditions c;
    c.latency = m_latencyMs / 1000.0;
    c.jitter = m_jitterMs / 1000.0;
    c.loss = m_lossPercent / 100.f;
    m_client->network.setConditions(c);
}

void NetDemo::mirrorGhosts(World& world) {
    auto& clientRt = m_client->services.get<gameplay::NetworkRuntime>();
    (void)clientRt;
    f32 errorSum = 0.f;
    u32 errorCount = 0;
    std::unordered_map<u32, Entity> authoritative;
    for (auto [e, id] : world.view<gameplay::NetworkIdentityComponent>().each()) authoritative[id.netId] = world.wrap(e);
    for (auto [e, id] : m_client->world.view<gameplay::NetworkIdentityComponent>().each()) {
        Entity replica = m_client->world.wrap(e);
        auto it = m_ghosts.find(id.netId);
        Entity ghost = it != m_ghosts.end() ? world.find(it->second) : Entity{};
        if (!ghost) {
            ghost = world.create("Ghost." + replica.name());
            auto& mr = ghost.add<MeshRendererComponent>();
            const bool crate = replica.name().find("Crate") != std::string::npos;
            mr.mesh = render::primitiveUuid(crate ? render::Primitive::Cube : render::Primitive::Capsule);
            mr.materials = {ghostMaterial()};
            mr.castShadows = false;
            m_ghosts[id.netId] = ghost.uuid();
        }
        Transform t = replica.worldTransform();
        const bool crate = replica.name().find("Crate") != std::string::npos;
        if (!crate) {
            t.position.y += 0.9f; // capsule centred on the character's feet position
            t.scale = glm::vec3(0.62f, 1.8f, 0.62f);
        }
        ghost.setWorldTransform(t);
        // How far the bot's interpolated view trails the authoritative object (latency + interpolation delay).
        if (auto a = authoritative.find(id.netId); a != authoritative.end() && !crate) {
            errorSum += glm::distance(a->second.worldPosition(), replica.worldPosition());
            ++errorCount;
        }
    }
    if (errorCount > 0) m_errorCm = m_errorCm * 0.9f + 0.1f * (errorSum / f32(errorCount)) * 100.f;
}

NetDemoStats NetDemo::stats() const {
    NetDemoStats s;
    s.latencyMs = m_latencyMs;
    s.lossPercent = m_lossPercent;
    if (!m_client) return s;
    s.running = true;
    auto& clientRt = const_cast<Client&>(*m_client).services.get<gameplay::NetworkRuntime>();
    if (const net::NetClient* c = clientRt.client()) {
        s.connected = c->connected();
        const net::PeerStats ps = c->stats();
        s.rttMs = ps.rttMs;
        s.receiveKBps = ps.receiveBandwidth / 1024.f;
        s.sendKBps = ps.sendBandwidth / 1024.f;
        s.tickRate = c->serverTickRate();
        s.interpolationDelayMs = 100.f;
    }
    if (auto* serverRt = m_module.engine().services().tryGet<gameplay::NetworkRuntime>()) {
        if (const net::NetServer* srv = serverRt->server(); srv && !srv->clients().empty()) {
            s.snapshotBytes = srv->lastSnapshotStats(srv->clients().front()).bytes;
        }
    }
    s.objects = u32(m_client->world.view<gameplay::NetworkIdentityComponent>().size());
    s.errorCm = m_errorCm;
    return s;
}

} // namespace ox::showcase
