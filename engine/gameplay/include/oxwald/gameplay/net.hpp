#pragma once

#include <oxwald/core/services.hpp>
#include <oxwald/gameplay/common.hpp>
#include <oxwald/net/net.hpp>
#include <oxwald/scene/world.hpp>

#include <entt/signal/sigh.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ox::gameplay {

// ---- components ------------------------------------------------------------------------------------------

// Replicates the entity from the server to clients. Clients spawn `netType` through IPrefabProvider (falling back
// to an empty entity). Every reflected field marked attr::Replicated on the entity's components is a replicated
// property (sorted by component name, then field order — server and client must have the same component set,
// which holds when both come from the same prefab).
struct NetworkIdentityComponent {
    std::string netType;
    net::Relevancy relevancy = net::Relevancy::Always;
    f32 relevancyRadius = 100.f;
    f32 priority = 1.f;
    bool viewer = false; // the owning client's viewer position (distance relevancy) follows this entity
    // runtime
    u32 netId = net::kInvalidNetId;
    u32 owner = net::kServerOwner; // set on the server before the entity is replicated
};

// Replicates the world transform. Clients interpolate (InterpolationBuffer at NetClient::renderTime()) unless
// the entity is owned by them and `predicted` (then the local simulation drives it and server states only
// correct errors larger than `correctionThreshold`).
struct NetworkTransformComponent {
    bool syncPosition = true;
    bool syncRotation = true;
    bool syncScale = false;
    f32 positionRange = 4096.f;      // +- metres covered by the quantised position
    f32 positionResolution = 0.01f;
    u32 rotationBits = 10;
    bool interpolate = true;
    bool predicted = false;
    f32 correctionThreshold = 0.5f;
};

// ---- runtime -------------------------------------------------------------------------------------------

enum class NetRole : u8 { None, Server, Client };

class NetworkRuntime {
public:
    NetworkRuntime();
    ~NetworkRuntime();
    NetworkRuntime(const NetworkRuntime&) = delete;
    NetworkRuntime& operator=(const NetworkRuntime&) = delete;

    bool startServer(std::unique_ptr<net::ITransport> transport, net::NetServerConfig config = {});
    bool connect(std::unique_ptr<net::ITransport> transport, std::string_view host, u16 port,
                 net::NetClientConfig config = {});
    void shutdown();

    [[nodiscard]] NetRole role() const { return m_role; }
    [[nodiscard]] net::NetServer* server() const { return m_server.get(); }
    [[nodiscard]] net::NetClient* client() const { return m_client.get(); }
    // Network clock (advanced by the systems with the frame delta unless setTime() is used).
    [[nodiscard]] f64 time() const { return m_time; }
    void setTime(f64 t) { m_time = t; m_externalTime = true; }

    // Client: makes `netType` spawnable (prefab names from IPrefabProvider are registered automatically).
    void registerNetType(const std::string& netType);
    [[nodiscard]] Entity entityOf(net::NetId id) const;

    // ---- driven by the gameplay systems ----
    void attach(World& world, Services& services);
    void detach();
    void preUpdate(f32 dt);  // poll, client interpolation
    void postUpdate(f32 dt); // server: spawn new identities, tick snapshots at the tick rate

private:
    class EntityObject;
    friend class EntityObject;
    std::shared_ptr<EntityObject> makeObject(Entity e, std::string_view netType);
    void serverAdd(Entity e);
    void onIdentityDestroyed(entt::registry& r, entt::entity e);
    void onIdentityConstructed(entt::registry& r, entt::entity e);
    void applyInterpolation();

    World* m_world = nullptr;
    Services* m_services = nullptr;
    NetRole m_role = NetRole::None;
    std::unique_ptr<net::NetServer> m_server;
    std::unique_ptr<net::NetClient> m_client;
    std::unordered_map<entt::entity, std::shared_ptr<EntityObject>> m_objects;
    std::unordered_map<net::NetId, entt::entity> m_byNetId;
    std::vector<entt::entity> m_pendingSpawns;
    std::vector<std::string> m_registeredTypes;
    std::vector<entt::scoped_connection> m_connections;
    f64 m_time = 0.0;
    f64 m_nextTick = 0.0;
    bool m_externalTime = false;
    bool m_despawning = false;
};

} // namespace ox::gameplay
