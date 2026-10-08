#include <oxwald/core/log.hpp>
#include <oxwald/core/profile.hpp>
#include <oxwald/gameplay/net.hpp>
#include <oxwald/gameplay/providers.hpp>
#include <oxwald/scene/component_registry.hpp>
#include <oxwald/scene/prefab.hpp>

#include <algorithm>

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
            if (glm::distance(e.worldPosition(), m_position) > m_correction) e.setWorldPosition(m_position);
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

} // namespace ox::gameplay
