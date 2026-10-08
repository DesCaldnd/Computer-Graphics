#include <oxwald/core/log.hpp>
#include <oxwald/core/profile.hpp>
#include <oxwald/gameplay/net.hpp>
#include <oxwald/gameplay/physics.hpp>
#include <oxwald/gameplay/providers.hpp>
#include <oxwald/scene/component_registry.hpp>
#include <oxwald/scene/prefab.hpp>

#include <algorithm>
#include <cmath>

namespace ox::gameplay {

using reflect::Kind;
using reflect::TypeInfo;
using reflect::ValueRef;

// NetObject bound to an entity: properties read/write components through reflection.
class NetworkRuntime::EntityObject final : public net::NetObject {
public:
    EntityObject(NetworkRuntime& rt, World& world, entt::entity e, std::string_view type)
        : NetObject(type), m_rt(rt), m_world(&world), m_entity(e) {}

    [[nodiscard]] Entity entity() const {
        return m_rt.m_world == m_world && m_world->valid(m_entity) ? m_world->wrap(m_entity) : Entity{};
    }

    void buildProperties() {
        const Entity e = entity();
        if (!e.valid()) return;
        if (const auto* nt = e.tryGet<NetworkTransformComponent>()) {
            m_hasTransform = true;
            m_interpolate = nt->interpolate;
            m_predicted = nt->predicted;
            m_correction = nt->correctionThreshold;
            const Transform wt = e.worldTransform();
            m_position = wt.position;
            m_rotation = wt.rotation;
            m_scale = wt.scale;
            if (nt->syncPosition) {
                replicate<glm::vec3, net::QuantizedVec3Codec>(
                    "transform.position", [this] { const Entity x = entity(); return x.valid() ? x.worldPosition() : m_position; },
                    [this](const glm::vec3& v) { m_position = v; },
                    net::QuantizedVec3Codec{-nt->positionRange, nt->positionRange, nt->positionResolution});
            }
            if (nt->syncRotation) {
                replicate<glm::quat, net::CompressedQuatCodec>(
                    "transform.rotation", [this] { const Entity x = entity(); return x.valid() ? x.worldRotation() : m_rotation; },
                    [this](const glm::quat& q) { m_rotation = q; }, net::CompressedQuatCodec{nt->rotationBits});
            }
            if (nt->syncScale) {
                replicate<glm::vec3>(
                    "transform.scale", [this] { const Entity x = entity(); return x.valid() ? x.worldTransform().scale : m_scale; },
                    [this](const glm::vec3& s) { m_scale = s; });
            }
        }
        // Reflected fields marked attr::Replicated, deterministic order: component name, then field order.
        std::vector<const ComponentInfo*> infos = ComponentRegistry::instance().componentsOf(*m_world, m_entity);
        std::sort(infos.begin(), infos.end(), [](const ComponentInfo* a, const ComponentInfo* b) { return a->name < b->name; });
        for (const ComponentInfo* info : infos) {
            for (const auto& f : info->type->fields) {
                if (f.attributes.replicated) addField(info, f.name, *f.type, f.attributes);
            }
        }
    }

    void onSnapshotApplied(f64 serverTime) override {
        if (!m_hasTransform) return;
        m_gotState = true;
        const Entity e = entity();
        if (!e.valid()) return;
        if (m_interpolate && !ownedPredicted()) {
            m_buffer.push({serverTime, m_position, m_rotation, std::nullopt});
        } else if (ownedPredicted()) {
            // PredictedCharacter entities reconcile through input replay (state acks), not snapshots.
            if (!m_rt.m_predictions.contains(m_entity) && glm::distance(e.worldPosition(), m_position) > m_correction) {
                e.setWorldPosition(m_position);
            }
        } else {
            applyTransform(e, m_position, m_rotation);
        }
    }

    void onNetDespawn() override {
        const Entity e = entity();
        m_rt.m_objects.erase(m_entity);
        m_rt.m_byNetId.erase(netId());
        if (e.valid()) e.destroy();
    }

    void interpolate(f64 renderTime) {
        if (!m_hasTransform || !m_interpolate || ownedPredicted()) return;
        const Entity e = entity();
        if (!e.valid()) return;
        if (auto s = m_buffer.sample(renderTime)) applyTransform(e, s->position, s->rotation);
    }

    [[nodiscard]] bool ownedPredicted() const {
        return m_predicted && m_rt.m_client && owner() != net::kServerOwner && owner() == m_rt.m_client->clientId();
    }

private:
    void applyTransform(Entity e, const glm::vec3& p, const glm::quat& q) {
        Transform wt = e.worldTransform();
        wt.position = p;
        wt.rotation = glm::normalize(q);
        if (const auto* nt = e.tryGet<NetworkTransformComponent>(); nt && nt->syncScale) wt.scale = m_scale;
        e.setWorldTransform(wt);
    }

    ValueRef fieldRef(const ComponentInfo* info, const std::string& path) const {
        const Entity e = entity();
        if (!e.valid() || !info->has(*m_world, m_entity)) return {};
        return reflect::resolvePath(info->ref(*m_world, m_entity), path);
    }

    template <class T, class Codec = net::NetCodec<T>>
    void addTyped(const ComponentInfo* info, const std::string& path, Codec codec = {}) {
        replicate<T, Codec>(
            info->name + "." + path,
            [this, info, path]() -> T {
                const ValueRef r = fieldRef(info, path);
                return r.valid() ? r.getAs<T>().value_or(T{}) : T{};
            },
            [this, info, path](const T& v) {
                const ValueRef r = fieldRef(info, path);
                if (r.valid()) r.setAs<T>(v);
            },
            std::move(codec));
    }

    void addField(const ComponentInfo* info, const std::string& path, const TypeInfo& t, const reflect::Attributes& a) {
        switch (t.kind) {
        case Kind::Bool: addTyped<bool>(info, path); break;
        case Kind::Int: addTyped<i64>(info, path); break;
        case Kind::UInt: addTyped<u64>(info, path); break;
        case Kind::Float:
            if (a.rangeMin && a.rangeMax && a.step && *a.step > 0.0) {
                addTyped<f32, net::QuantizedFloatCodec>(
                    info, path,
                    net::QuantizedFloatCodec{static_cast<f32>(*a.rangeMin), static_cast<f32>(*a.rangeMax),
                                             static_cast<f32>(*a.step)});
            } else {
                addTyped<f32>(info, path);
            }
            break;
        case Kind::String: addTyped<std::string>(info, path); break;
        case Kind::Enum:
            replicate<i64>(
                info->name + "." + path,
                [this, info, path]() -> i64 {
                    const ValueRef r = fieldRef(info, path);
                    return r.valid() ? r.type->getEnum(r.ptr) : 0;
                },
                [this, info, path](const i64& v) {
                    const ValueRef r = fieldRef(info, path);
                    if (r.valid()) r.type->setEnum(r.ptr, v);
                });
            break;
        case Kind::Math:
            switch (t.tag) {
            case serial::Tag::Vec2: addTyped<glm::vec2>(info, path); break;
            case serial::Tag::Vec3: addTyped<glm::vec3>(info, path); break;
            case serial::Tag::Vec4: addTyped<glm::vec4>(info, path); break;
            case serial::Tag::Quat: addTyped<glm::quat, net::CompressedQuatCodec>(info, path, net::CompressedQuatCodec{12}); break;
            default: OX_LOG_WARN("gameplay", "replication: unsupported math field {}.{}", info->name, path); break;
            }
            break;
        case Kind::Custom:
            if (t.tag == serial::Tag::EntityRef) {
                // Entity references travel as net ids (UUIDs differ between server and client instances).
                replicate<u32>(
                    info->name + "." + path,
                    [this, info, path]() -> u32 {
                        const ValueRef r = fieldRef(info, path);
                        const auto* ref = r.valid() ? static_cast<const EntityRef*>(r.ptr) : nullptr;
                        const Entity target = ref ? m_world->resolve(*ref) : Entity{};
                        const auto* id = target.valid() ? target.tryGet<NetworkIdentityComponent>() : nullptr;
                        return id ? id->netId : net::kInvalidNetId;
                    },
                    [this, info, path](const u32& netId) {
                        const ValueRef r = fieldRef(info, path);
                        if (!r.valid()) return;
                        const Entity target = m_rt.entityOf(netId);
                        *static_cast<EntityRef*>(r.ptr) = target.valid() ? target.ref() : EntityRef{};
                    });
                break;
            }
            [[fallthrough]];
        case Kind::Struct:
            if (t.kind == Kind::Struct) {
                for (const auto& f : t.fields) addField(info, path + "." + f.name, *f.type, f.attributes);
                break;
            }
            [[fallthrough]];
        default:
            OX_LOG_WARN("gameplay", "replication: field {}.{} ({}) is not supported", info->name, path, t.name);
            break;
        }
    }

    NetworkRuntime& m_rt;
    World* m_world;
    entt::entity m_entity;
    bool m_hasTransform = false;
    bool m_interpolate = true;
    bool m_predicted = false;
    bool m_gotState = false;
    f32 m_correction = 0.5f;
    glm::vec3 m_position{0.f};
    glm::quat m_rotation{1.f, 0.f, 0.f, 0.f};
    glm::vec3 m_scale{1.f};
    net::InterpolationBuffer m_buffer;

    friend class NetworkRuntime;
};

NetworkRuntime::NetworkRuntime() = default;
NetworkRuntime::~NetworkRuntime() {
    shutdown();
    detach();
}

void NetworkRuntime::attach(World& world, Services& services) {
    detach();
    m_world = &world;
    m_services = &services;
    entt::registry& r = world.registry();
    m_connections.emplace_back(
        r.on_construct<NetworkIdentityComponent>().connect<&NetworkRuntime::onIdentityConstructed>(*this));
    m_connections.emplace_back(
        r.on_destroy<NetworkIdentityComponent>().connect<&NetworkRuntime::onIdentityDestroyed>(*this));
    if (m_role == NetRole::Server) {
        for (auto e : r.view<NetworkIdentityComponent>()) m_pendingSpawns.push_back(e);
    }
}

void NetworkRuntime::detach() {
    if (!m_world) return;
    if (m_server) {
        for (auto& [e, obj] : m_objects) {
            if (obj->netId() != net::kInvalidNetId) m_server->replication().remove(obj->netId());
        }
    }
    while (!m_predictions.empty()) dropPrediction(m_predictions.begin()->first);
    m_objects.clear();
    m_byNetId.clear();
    m_pendingSpawns.clear();
    m_connections.clear();
    m_world = nullptr;
}

bool NetworkRuntime::startServer(std::unique_ptr<net::ITransport> transport, net::NetServerConfig config) {
    shutdown();
    m_server = std::make_unique<net::NetServer>(std::move(transport), std::move(config));
    if (!m_server->start()) {
        OX_LOG_ERROR("gameplay", "network server failed to start");
        m_server.reset();
        return false;
    }
    m_role = NetRole::Server;
    m_nextTick = m_time;
    installPredictionHandlers();
    if (m_world) {
        for (auto e : m_world->registry().view<NetworkIdentityComponent>()) m_pendingSpawns.push_back(e);
    }
    return true;
}

bool NetworkRuntime::connect(std::unique_ptr<net::ITransport> transport, std::string_view host, u16 port,
                             net::NetClientConfig config) {
    shutdown();
    m_client = std::make_unique<net::NetClient>(std::move(transport), config);
    m_role = NetRole::Client;
    installPredictionHandlers();
    std::vector<std::string> types = m_registeredTypes;
    m_registeredTypes.clear();
    for (const auto& t : types) registerNetType(t);
    if (auto* prefabs = m_services ? m_services->tryGet<IPrefabProvider>() : nullptr) {
        for (const auto& name : prefabs->prefabNames()) registerNetType(name);
    }
    if (!m_client->connect(host, port)) {
        OX_LOG_ERROR("gameplay", "network client could not connect to {}:{}", host, port);
        m_client.reset();
        m_role = NetRole::None;
        return false;
    }
    return true;
}

void NetworkRuntime::shutdown() {
    if (m_client) m_client->disconnect();
    if (m_server) m_server->stop();
    // Client-side replicated entities stay in the world; their objects are dropped.
    while (!m_predictions.empty()) dropPrediction(m_predictions.begin()->first);
    m_objects.clear();
    m_byNetId.clear();
    m_client.reset();
    m_server.reset();
    m_role = NetRole::None;
}

void NetworkRuntime::registerNetType(const std::string& netType) {
    if (std::find(m_registeredTypes.begin(), m_registeredTypes.end(), netType) != m_registeredTypes.end()) return;
    m_registeredTypes.push_back(netType);
    if (!m_client) return;
    m_client->replication().factory().registerType(
        netType, [this, netType](net::NetTypeId, net::NetId id, net::PeerId owner) -> std::shared_ptr<net::NetObject> {
            if (!m_world) return nullptr;
            Entity root;
            if (auto* prefabs = m_services ? m_services->tryGet<IPrefabProvider>() : nullptr) {
                if (auto doc = prefabs->prefab(netType)) {
                    if (auto inst = instantiatePrefab(*m_world, *doc)) root = *inst;
                }
            }
            if (!root.valid()) root = m_world->create(netType);
            auto& ident = root.has<NetworkIdentityComponent>() ? root.get<NetworkIdentityComponent>()
                                                               : root.add<NetworkIdentityComponent>();
            ident.netType = netType;
            ident.netId = id;
            ident.owner = owner;
            auto obj = makeObject(root, netType);
            obj->setOwner(owner);
            if (!obj->ownedPredicted() && !root.has<NetworkProxyTag>()) m_world->registry().emplace<NetworkProxyTag>(root.handle());
            m_objects[root.handle()] = obj;
            m_byNetId[id] = root.handle();
            return obj;
        });
}

std::shared_ptr<NetworkRuntime::EntityObject> NetworkRuntime::makeObject(Entity e, std::string_view netType) {
    auto obj = std::make_shared<EntityObject>(*this, *m_world, e.handle(), netType);
    obj->buildProperties();
    return obj;
}

Entity NetworkRuntime::entityOf(net::NetId id) const {
    auto it = m_byNetId.find(id);
    return it != m_byNetId.end() && m_world && m_world->valid(it->second) ? m_world->wrap(it->second) : Entity{};
}

void NetworkRuntime::onIdentityConstructed(entt::registry&, entt::entity e) {
    if (m_role == NetRole::Server) m_pendingSpawns.push_back(e);
}

void NetworkRuntime::onIdentityDestroyed(entt::registry& r, entt::entity e) {
    m_predictions.erase(e); // entity is going away: no tag to remove
    auto it = m_objects.find(e);
    if (it == m_objects.end()) return;
    const net::NetId id = it->second->netId();
    if (m_server && id != net::kInvalidNetId) m_server->replication().remove(id);
    m_byNetId.erase(id);
    m_objects.erase(it);
    (void)r;
}

void NetworkRuntime::serverAdd(Entity e) {
    auto* ident = e.tryGet<NetworkIdentityComponent>();
    if (!ident || m_objects.contains(e.handle())) return;
    auto obj = makeObject(e, ident->netType.empty() ? std::string_view(e.name()) : std::string_view(ident->netType));
    obj->setOwner(ident->owner);
    obj->setRelevancy(ident->relevancy, ident->relevancyRadius);
    obj->setPriority(ident->priority);
    obj->setPositionSource([this, h = e.handle()] {
        return m_world && m_world->valid(h) ? m_world->wrap(h).worldPosition() : glm::vec3(0.f);
    });
    const net::NetId id = m_server->replication().add(obj);
    ident->netId = id;
    m_objects[e.handle()] = obj;
    m_byNetId[id] = e.handle();
}

void NetworkRuntime::preUpdate(f32 dt) {
    if (!m_externalTime) m_time += dt;
    if (!m_world) return;
    OX_PROFILE_ZONE_N("NetworkRuntime::preUpdate");
    if (m_server) m_server->poll(m_time);
    if (m_client) {
        m_client->poll(m_time);
        applyInterpolation();
    }
}

void NetworkRuntime::applyInterpolation() {
    if (!m_client || !m_client->connected()) return;
    const f64 renderTime = m_client->renderTime();
    for (auto& [e, obj] : m_objects) obj->interpolate(renderTime);
}

void NetworkRuntime::postUpdate(f32) {
    if (!m_world || !m_server) return;
    OX_PROFILE_ZONE_N("NetworkRuntime::postUpdate");
    if (!m_pendingSpawns.empty()) {
        std::vector<entt::entity> pending;
        pending.swap(m_pendingSpawns);
        std::sort(pending.begin(), pending.end());
        pending.erase(std::unique(pending.begin(), pending.end()), pending.end());
        entt::registry& r = m_world->registry();
        for (auto e : pending) {
            if (r.valid(e) && r.all_of<NetworkIdentityComponent>(e) && !r.all_of<PendingDestroyTag>(e)) serverAdd(m_world->wrap(e));
        }
    }
    for (auto [handle, ident] : m_world->registry().view<NetworkIdentityComponent>().each()) {
        if (ident.viewer && ident.owner != net::kServerOwner) {
            m_server->replication().setViewerPosition(ident.owner, m_world->wrap(handle).worldPosition());
        }
    }
    const f64 interval = 1.0 / std::max(1.f, m_server->tickRate());
    if (m_time + 1e-9 >= m_nextTick) {
        m_server->tick(m_time);
        m_nextTick += interval;
        if (m_nextTick < m_time) m_nextTick = m_time + interval; // fell behind: don't burst
    }
}

// ---- client-side prediction with input replay ------------------------------------------------------------

namespace {

struct PredictedState {
    glm::vec3 position{0.f};
    glm::vec3 velocity{0.f};
};

constexpr u32 kMaxInputsPerBatch = 64;
constexpr u32 kMaxServerInputsPerStep = 4; // catch up after jitter bursts without fast-forwarding a whole backlog

// Client -> server: newest unacknowledged inputs of one predicted entity (redundant, unreliable).
struct PredictedInputBatch {
    static constexpr std::string_view kNetName = "ox.predict.inputs";
    struct Entry {
        u32 seq = 0;
        CharacterInput input;
        f32 dt = 0.f;
    };
    u32 netId = net::kInvalidNetId;
    std::vector<Entry> entries;

    void serialize(net::BitWriter& w) const {
        w.writeVarU32(netId);
        w.writeVarU32(static_cast<u32>(entries.size()));
        for (const Entry& e : entries) {
            w.writeVarU32(e.seq);
            w.writeF32(e.input.move.x);
            w.writeF32(e.input.move.y);
            w.writeF32(e.input.yaw);
            w.writeBool(e.input.jump);
            w.writeF32(e.dt);
        }
    }
    void deserialize(net::BitReader& r) {
        netId = r.readVarU32();
        const u32 n = std::min(r.readVarU32(), kMaxInputsPerBatch);
        entries.resize(n);
        for (Entry& e : entries) {
            e.seq = r.readVarU32();
            e.input.move.x = r.readF32();
            e.input.move.y = r.readF32();
            e.input.yaw = r.readF32();
            e.input.jump = r.readBool();
            e.dt = r.readF32();
        }
    }
};

// Server -> owning client: authoritative state after input `seq` (exact floats: replay compares against it).
struct PredictedStateAck {
    static constexpr std::string_view kNetName = "ox.predict.ack";
    u32 netId = net::kInvalidNetId;
    u32 seq = 0;
    PredictedState state;

    void serialize(net::BitWriter& w) const {
        w.writeVarU32(netId);
        w.writeVarU32(seq);
        w.writeVec3(state.position);
        w.writeVec3(state.velocity);
    }
    void deserialize(net::BitReader& r) {
        netId = r.readVarU32();
        seq = r.readVarU32();
        state.position = r.readVec3();
        state.velocity = r.readVec3();
    }
};

CharacterInput sanitize(CharacterInput in) {
    if (!std::isfinite(in.move.x) || !std::isfinite(in.move.y)) in.move = glm::vec2(0.f);
    if (!std::isfinite(in.yaw)) in.yaw = 0.f;
    const f32 len = glm::length(in.move);
    if (len > 1.f) in.move /= len;
    return in;
}

} // namespace

glm::vec3 desiredCharacterVelocity(const CharacterInput& input, f32 speed) {
    const CharacterInput in = sanitize(input);
    const f32 s = std::sin(in.yaw), c = std::cos(in.yaw);
    const glm::vec3 forward(-s, 0.f, -c); // yaw 0 = -Z
    const glm::vec3 right(c, 0.f, -s);
    return (right * in.move.x + forward * in.move.y) * speed;
}

struct NetworkRuntime::Prediction {
    entt::entity entity = entt::null;
    bool server = false;
    std::unique_ptr<net::ClientPrediction<PredictedState, CharacterInput>> client;
    net::ServerInputQueue<CharacterInput> queue{256};
    u32 lastAcked = 0; // server: last seq acknowledged to the client
    bool ackDue = false;
    // Bound each step before simulating (the character can be recreated, e.g. after a collider change).
    PhysicsRuntime* physics = nullptr;
    physics::CharacterHandle character;
    const CharacterControllerComponent* controller = nullptr;
    f32 speed = 5.f;

    PredictedState read() const {
        const physics::CharacterState s = physics->physicsWorld().getCharacterState(character);
        return {s.position, s.linearVelocity};
    }
    // The one simulation step shared by prediction, replay and the server.
    void simulate(PredictedState& st, const CharacterInput& in, f32 dt) const {
        if (!physics || !character) return;
        auto& pw = physics->physicsWorld();
        const physics::CharacterState cur = pw.getCharacterState(character);
        if (cur.position != st.position) pw.setCharacterTransform(character, st.position, cur.rotation);
        if (cur.linearVelocity != st.velocity) pw.setCharacterVelocity(character, st.velocity);
        physics::CharacterMoveInput mi;
        mi.desiredVelocity = desiredCharacterVelocity(in, speed);
        mi.jump = in.jump;
        if (controller) {
            mi.jumpSpeed = controller->jumpSpeed;
            mi.airControl = controller->airControl;
        }
        pw.moveCharacter(character, dt, mi);
        st = read();
    }
};

void NetworkRuntime::installPredictionHandlers() {
    if (m_server) {
        m_server->messages().on<PredictedInputBatch>([this](net::PeerId from, const PredictedInputBatch& batch) {
            const Entity e = entityOf(batch.netId);
            if (!e.valid()) return;
            const auto* ident = e.tryGet<NetworkIdentityComponent>();
            if (!ident || ident->owner != from || !e.has<PredictedCharacterComponent>()) return; // not theirs
            Prediction* p = ensurePrediction(e, true);
            if (!p) return;
            for (const auto& entry : batch.entries) {
                const f32 dt = std::isfinite(entry.dt) ? std::clamp(entry.dt, 0.f, 0.1f) : 0.f; // no speed hacks
                p->queue.receive(entry.seq, sanitize(entry.input), dt);
            }
        });
    }
    if (m_client) {
        m_client->messages().on<PredictedStateAck>([this](net::PeerId, const PredictedStateAck& ack) {
            const Entity e = entityOf(ack.netId);
            auto it = e.valid() ? m_predictions.find(e.handle()) : m_predictions.end();
            if (it == m_predictions.end() || !it->second->client) return;
            Prediction& p = *it->second;
            auto* pc = e.tryGet<PredictedCharacterComponent>();
            auto* physicsRt = m_services ? m_services->tryGet<PhysicsRuntime>() : nullptr;
            if (!pc || !physicsRt) return;
            p.physics = physicsRt;
            p.character = physicsRt->characterOf(e);
            p.controller = e.tryGet<CharacterControllerComponent>();
            p.speed = pc->moveSpeed;
            if (!p.character) return;
            if (p.client->reconcile(ack.seq, ack.state)) {
                ++pc->corrections;
                Transform wt = e.worldTransform();
                wt.position = p.client->state().position;
                e.setWorldTransform(wt);
            }
            pc->pendingInputs = static_cast<u32>(p.client->pending().size());
        });
    }
}

NetworkRuntime::Prediction* NetworkRuntime::ensurePrediction(Entity e, bool server) {
    auto it = m_predictions.find(e.handle());
    if (it != m_predictions.end()) return it->second->server == server ? it->second.get() : nullptr;
    auto* physicsRt = m_services ? m_services->tryGet<PhysicsRuntime>() : nullptr;
    if (!physicsRt || !e.has<CharacterControllerComponent>()) return nullptr;
    if (!physicsRt->characterOf(e)) physicsRt->processPending(); // spawned this frame: create the character now
    const physics::CharacterHandle ch = physicsRt->characterOf(e);
    if (!ch) return nullptr;
    auto p = std::make_unique<Prediction>();
    p->entity = e.handle();
    p->server = server;
    p->physics = physicsRt;
    p->character = ch;
    p->controller = e.tryGet<CharacterControllerComponent>();
    if (!server) {
        const f32 tolerance = e.get<PredictedCharacterComponent>().correctionTolerance;
        Prediction* raw = p.get();
        p->client = std::make_unique<net::ClientPrediction<PredictedState, CharacterInput>>(
            [raw](PredictedState& st, const CharacterInput& in, f32 dt) { raw->simulate(st, in, dt); },
            [tolerance](const PredictedState& a, const PredictedState& b) {
                return glm::distance(a.position, b.position) <= tolerance &&
                       glm::distance(a.velocity, b.velocity) <= tolerance * 20.f;
            },
            p->read());
    }
    m_world->registry().emplace_or_replace<ExternalCharacterMotionTag>(e.handle());
    return m_predictions.emplace(e.handle(), std::move(p)).first->second.get();
}

void NetworkRuntime::dropPrediction(entt::entity e) {
    m_predictions.erase(e);
    if (m_world && m_world->valid(e)) m_world->registry().remove<ExternalCharacterMotionTag>(e);
}

bool NetworkRuntime::isPredicted(Entity e) const { return e.valid() && m_predictions.contains(e.handle()); }

void NetworkRuntime::fixedUpdate(f32 dt) {
    if (!m_world) return;
    OX_PROFILE_ZONE_N("NetworkRuntime::fixedUpdate");
    predictionStep(dt);
}

void NetworkRuntime::predictionStep(f32 dt) {
    entt::registry& r = m_world->registry();
    auto* source = m_services ? m_services->tryGet<ICharacterInputSource>() : nullptr;
    auto* physicsRt = m_services ? m_services->tryGet<PhysicsRuntime>() : nullptr;

    // Drop records of entities that lost their components / ownership.
    std::vector<entt::entity> stale;
    for (auto& [e, p] : m_predictions) {
        if (!r.valid(e) || !r.all_of<PredictedCharacterComponent, CharacterControllerComponent>(e)) stale.push_back(e);
    }
    for (auto e : stale) dropPrediction(e);

    std::vector<entt::entity> entities(r.view<PredictedCharacterComponent>().begin(), r.view<PredictedCharacterComponent>().end());
    for (entt::entity handle : entities) {
        if (!r.valid(handle) || r.all_of<PendingDestroyTag>(handle) || !r.all_of<CharacterControllerComponent>(handle)) continue;
        const Entity e = m_world->wrap(handle);
        if (!e.activeInHierarchy()) continue;
        auto& pc = r.get<PredictedCharacterComponent>(handle);
        const auto* ident = r.try_get<NetworkIdentityComponent>(handle);
        auto obj = m_objects.find(handle);

        if (m_role == NetRole::Client) {
            if (obj == m_objects.end() || !obj->second->ownedPredicted() || obj->second->netId() == net::kInvalidNetId) continue;
            if (!m_client->connected()) continue;
            Prediction* p = ensurePrediction(e, false);
            if (!p) continue;
            p->physics = physicsRt;
            p->character = physicsRt ? physicsRt->characterOf(e) : physics::CharacterHandle{};
            p->controller = r.try_get<CharacterControllerComponent>(handle);
            p->speed = pc.moveSpeed;
            if (!p->character) continue;
            const CharacterInput input = sanitize(source ? source->sample(e, pc, dt) : CharacterInput{});
            p->client->applyInput(input, dt);
            Transform wt = e.worldTransform();
            wt.position = p->client->state().position;
            e.setWorldTransform(wt);

            PredictedInputBatch batch;
            batch.netId = obj->second->netId();
            const auto& pending = p->client->pending();
            const usize count = std::min<usize>(pending.size(), std::clamp<u32>(pc.inputRedundancy, 1u, kMaxInputsPerBatch));
            for (usize i = pending.size() - count; i < pending.size(); ++i) {
                batch.entries.push_back({pending[i].seq, pending[i].input, pending[i].dt});
            }
            m_client->send(batch, net::Channel::UnreliableSequenced); // newest batch wins; it repeats older inputs
            pc.pendingInputs = static_cast<u32>(pending.size());
            continue;
        }

        const bool remoteOwned = m_role == NetRole::Server && ident && ident->owner != net::kServerOwner;
        if (remoteOwned) {
            Prediction* p = ensurePrediction(e, true);
            if (!p) continue;
            p->physics = physicsRt;
            p->character = physicsRt ? physicsRt->characterOf(e) : physics::CharacterHandle{};
            p->controller = r.try_get<CharacterControllerComponent>(handle);
            p->speed = pc.moveSpeed;
            if (!p->character) continue;
            // Gameplay code (or an editor) moved the authoritative character: start from there.
            if (const glm::vec3 wp = e.worldPosition(); glm::distance(wp, p->read().position) > 1e-4f) {
                p->physics->physicsWorld().setCharacterTransform(p->character, wp, glm::normalize(e.worldRotation()));
            }
            PredictedState st = p->read();
            const u32 ran = p->queue.process([&](const CharacterInput& in, f32 inputDt) { p->simulate(st, in, inputDt); },
                                             kMaxServerInputsPerStep);
            if (ran > 0) {
                Transform wt = e.worldTransform();
                wt.position = st.position;
                e.setWorldTransform(wt);
                p->ackDue = true;
            }
            if (p->ackDue && obj != m_objects.end() && obj->second->netId() != net::kInvalidNetId) {
                PredictedStateAck ack;
                ack.netId = obj->second->netId();
                ack.seq = p->queue.lastProcessed();
                ack.state = st;
                m_server->send(ident->owner, ack, net::Channel::UnreliableSequenced);
                p->lastAcked = ack.seq;
                p->ackDue = false;
            }
            continue;
        }

        // Offline / listen-server host: the local input drives the controller like any gameplay code would.
        if (m_predictions.contains(handle)) dropPrediction(handle);
        if (source && (m_role == NetRole::None || (m_role == NetRole::Server && ident && ident->owner == net::kServerOwner) ||
                       (m_role == NetRole::Server && !ident))) {
            const CharacterInput input = sanitize(source->sample(e, pc, dt));
            auto& cc = r.get<CharacterControllerComponent>(handle);
            cc.desiredVelocity = desiredCharacterVelocity(input, pc.moveSpeed);
            if (input.jump) cc.jump = true;
        }
    }
}

} // namespace ox::gameplay
