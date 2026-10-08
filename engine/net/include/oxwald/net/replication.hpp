#pragma once

#include <oxwald/net/serialize.hpp>
#include <oxwald/net/transport.hpp>

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ox::net {

using NetId = u32; // never reused within a server session
inline constexpr NetId kInvalidNetId = 0;
using NetTypeId = u32; // netHash(type name)
inline constexpr PeerId kServerOwner = kInvalidPeer;

// Captured property value. Values are immutable once captured, so states can be shared between clients and
// snapshot history entries without copying.
using NetValue = std::shared_ptr<const void>;
using NetState = std::vector<NetValue>;
using NetStatePtr = std::shared_ptr<const NetState>;

// Type-erased replicated property. Getter/setter must be symmetric: set(get()) is a no-op.
class IReplicatedProperty {
public:
    virtual ~IReplicatedProperty() = default;
    virtual const std::string& name() const = 0;
    virtual NetValue capture() const = 0;
    virtual bool equal(const void* a, const void* b) const = 0;
    virtual void write(BitWriter& w, const void* value) const = 0;
    virtual NetValue read(BitReader& r) const = 0;
    virtual void apply(const void* value) = 0;
};

template <class T, class Codec>
class ReplicatedProperty final : public IReplicatedProperty {
public:
    ReplicatedProperty(std::string name, std::function<T()> get, std::function<void(const T&)> set, Codec codec)
        : m_name(std::move(name)), m_get(std::move(get)), m_set(std::move(set)), m_codec(std::move(codec)) {}

    const std::string& name() const override { return m_name; }
    NetValue capture() const override { return std::make_shared<const T>(m_get()); }
    bool equal(const void* a, const void* b) const override {
        return m_codec.equal(*static_cast<const T*>(a), *static_cast<const T*>(b));
    }
    void write(BitWriter& w, const void* value) const override { m_codec.write(w, *static_cast<const T*>(value)); }
    NetValue read(BitReader& r) const override {
        auto v = std::make_shared<T>();
        m_codec.read(r, *v);
        return v;
    }
    void apply(const void* value) override { m_set(*static_cast<const T*>(value)); }

private:
    std::string m_name;
    std::function<T()> m_get;
    std::function<void(const T&)> m_set;
    Codec m_codec;
};

enum class Relevancy : u8 {
    Always,    // every client
    Distance,  // clients whose viewer position is within relevancyRadius of positionSource()
    OwnerOnly, // only the owning client
    Custom,    // relevancyFilter decides
};

// A replicated object. Subclass it (factory creates the same subclass on clients) or use it directly with
// getter/setter lambdas bound to external data (ECS components — see docs/dev/modules/net.md).
// Properties must be registered in the same order on server and client.
class NetObject {
public:
    explicit NetObject(std::string_view typeName);
    virtual ~NetObject();
    NetObject(const NetObject&) = delete;
    NetObject& operator=(const NetObject&) = delete;

    NetId netId() const { return m_netId; }
    NetTypeId typeId() const { return m_typeId; }
    const std::string& typeName() const { return m_typeName; }
    PeerId owner() const { return m_owner; }
    void setOwner(PeerId owner) { m_owner = owner; } // server side, before add(); transfer is a TODO
    bool isOwnedBy(PeerId peer) const { return m_owner != kServerOwner && m_owner == peer; }

    void setRelevancy(Relevancy mode, f32 radius = 0.f) {
        m_relevancy = mode;
        m_relevancyRadius = radius;
    }
    Relevancy relevancy() const { return m_relevancy; }
    void setPositionSource(std::function<glm::vec3()> fn) { m_position = std::move(fn); }
    void setRelevancyFilter(std::function<bool(PeerId client, const glm::vec3* viewer)> fn) {
        m_filter = std::move(fn);
    }
    // Base priority; the per-client accumulator grows by priority * distance factor each tick the object is dirty.
    void setPriority(f32 priority) { m_priority = priority; }
    f32 priority() const { return m_priority; }

    bool isRelevantTo(PeerId client, const glm::vec3* viewer) const;
    glm::vec3 position() const { return m_position ? m_position() : glm::vec3(0.f); }
    bool hasPosition() const { return static_cast<bool>(m_position); }

    template <class T, class Codec = NetCodec<T>>
        requires NetCodecFor<Codec, T>
    void replicate(std::string_view name, std::function<T()> get, std::function<void(const T&)> set,
                   Codec codec = {}) {
        m_properties.push_back(std::make_unique<ReplicatedProperty<T, Codec>>(std::string(name), std::move(get),
                                                                              std::move(set), std::move(codec)));
    }

    // Binds a member field directly. The field must outlive the NetObject (normally it is a member).
    template <class T, class Codec = NetCodec<T>>
        requires NetCodecFor<Codec, T>
    void replicate(std::string_view name, T& field, Codec codec = {}) {
        replicate<T, Codec>(name, [&field] { return field; }, [&field](const T& v) { field = v; }, std::move(codec));
    }

    const std::vector<std::unique_ptr<IReplicatedProperty>>& properties() const { return m_properties; }
    NetState captureState() const;

    // Client-side hooks.
    virtual void onNetSpawn() {}
    virtual void onNetDespawn() {}
    // Called after a snapshot touched this object (at least one property applied or not). serverTime is the
    // server clock of that snapshot: push interpolation samples here.
    virtual void onSnapshotApplied(f64 serverTime) {}

private:
    friend class ReplicationServer;
    friend class ReplicationClient;

    std::string m_typeName;
    NetTypeId m_typeId;
    NetId m_netId = kInvalidNetId;
    PeerId m_owner = kServerOwner;
    Relevancy m_relevancy = Relevancy::Always;
    f32 m_relevancyRadius = 0.f;
    f32 m_priority = 1.f;
    std::function<glm::vec3()> m_position;
    std::function<bool(PeerId, const glm::vec3*)> m_filter;
    std::vector<std::unique_ptr<IReplicatedProperty>> m_properties;
};

struct ReplicationConfig {
    u32 bytesPerTick = 1200;     // per client snapshot budget (bandwidth / tick rate)
    u32 maxBaselineAge = 32;     // older acked baselines are not used; full state is sent instead
    u32 sentHistory = 128;       // per client, packets remembered for ack processing
    u32 maxDespawnsPerPacket = 64;
};

struct SnapshotStats {
    u32 bytes = 0;
    u32 objectsWritten = 0;
    u32 objectsDeferred = 0; // dirty but skipped because of the budget
    u32 spawns = 0;
    u32 despawns = 0;
};

// Server side: tracks objects and, per client, what was acked; writes delta snapshots under a byte budget.
class ReplicationServer {
public:
    explicit ReplicationServer(ReplicationConfig config = {});
    ~ReplicationServer();

    NetId add(std::shared_ptr<NetObject> object);
    void remove(NetId id); // despawns on all clients
    NetObject* find(NetId id) const;
    usize objectCount() const { return m_objects.size(); }

    void addClient(PeerId client);
    void removeClient(PeerId client);
    void setViewerPosition(PeerId client, const glm::vec3& position);
    void setConfig(const ReplicationConfig& config) { m_config = config; }
    const ReplicationConfig& config() const { return m_config; }

    // Capture every object's current state once per tick (shared by all clients).
    void beginTick();
    // Writes one snapshot for `client` (header + despawns + objects). Unreliable channel.
    SnapshotStats writeSnapshot(PeerId client, BitWriter& w, u32 serverTick, f64 serverTime);
    void processAck(PeerId client, u32 latestSeq, u32 ackBits);

    bool isSpawnedOn(PeerId client, NetId id) const; // spawn acknowledged by the client

private:
    struct ClientRecord;
    struct ClientState;
    ReplicationConfig m_config;
    NetId m_nextId = 1;
    std::unordered_map<NetId, std::shared_ptr<NetObject>> m_objects;
    std::vector<NetId> m_order; // stable iteration order
    std::unordered_map<NetId, NetStatePtr> m_current;
    std::unordered_map<PeerId, std::unique_ptr<ClientState>> m_clients;
};

// Creates client-side objects for spawn messages.
class NetObjectFactory {
public:
    using CreateFn = std::function<std::shared_ptr<NetObject>(NetTypeId type, NetId id, PeerId owner)>;

    void registerType(std::string_view typeName, CreateFn fn);
    template <class T>
    void registerType(std::string_view typeName) {
        registerType(typeName, [](NetTypeId, NetId, PeerId) { return std::make_shared<T>(); });
    }
    std::shared_ptr<NetObject> create(NetTypeId type, NetId id, PeerId owner) const;
    bool contains(NetTypeId type) const { return m_types.contains(type); }

private:
    std::unordered_map<NetTypeId, CreateFn> m_types;
};

// Client side: applies snapshots, keeps per-object state history for delta baselines, produces acks.
class ReplicationClient {
public:
    ReplicationClient();
    ~ReplicationClient();

    NetObjectFactory& factory() { return m_factory; }
    void setLocalPeer(PeerId serverAssignedId) { m_localPeer = serverAssignedId; }
    PeerId localPeer() const { return m_localPeer; }

    // Returns false when the snapshot was stale (older than the newest applied) or malformed.
    bool readSnapshot(BitReader& r);
    // Ack of the newest applied snapshot + bitfield of the 32 before it.
    void writeAck(BitWriter& w) const;
    bool hasAckToSend() const { return m_ackDirty; }
    void clearAckDirty() { m_ackDirty = false; }

    NetObject* find(NetId id) const;
    const std::unordered_map<NetId, std::shared_ptr<NetObject>>& objects() const { return m_objects; }
    bool isOwned(const NetObject& object) const { return object.isOwnedBy(m_localPeer); }

    u32 lastSnapshotSeq() const { return m_latestSeq; }
    u32 lastServerTick() const { return m_lastTick; }
    f64 lastServerTime() const { return m_lastServerTime; }

    std::function<void(NetObject&)> onSpawn;
    std::function<void(NetObject&)> onDespawn;

    void clear();

private:
    struct History {
        std::deque<std::pair<u32, NetStatePtr>> states;
    };
    void despawn(NetId id);

    NetObjectFactory m_factory;
    PeerId m_localPeer = kInvalidPeer;
    std::unordered_map<NetId, std::shared_ptr<NetObject>> m_objects;
    std::unordered_map<NetId, History> m_history;
    u32 m_latestSeq = 0;
    u32 m_ackBits = 0;
    bool m_ackDirty = false;
    u32 m_lastTick = 0;
    f64 m_lastServerTime = 0.0;
};

} // namespace ox::net
