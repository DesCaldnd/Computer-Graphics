#pragma once

#include <oxwald/net/messages.hpp>
#include <oxwald/net/replication.hpp>
#include <oxwald/net/transport.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace ox::net {

// First byte of every packet produced by NetServer/NetClient.
enum class PacketKind : u8 { Welcome = 1, Message, Rpc, Snapshot, SnapshotAck, ClockPing, ClockPong, User = 32 };

struct NetServerConfig {
    ListenConfig listen;
    u32 protocolVersion = 1; // clients with another version are rejected (DisconnectReason::VersionMismatch)
    u32 maxClients = 32;
    f32 tickRate = 30.f;
    ReplicationConfig replication;
};

// Authoritative server: client bookkeeping, messages, RPCs, replication snapshots, clock sync replies.
// Call poll(now) often (every frame) and tick(now) at the fixed tick rate.
class NetServer {
public:
    NetServer(std::unique_ptr<ITransport> transport, NetServerConfig config = {});
    ~NetServer();

    bool start();
    void stop();
    bool running() const { return m_running; }
    u16 port() const { return m_transport->localPort(); }

    void poll(f64 now);
    // Advances the tick counter and sends one snapshot to each client.
    void tick(f64 now);
    u32 currentTick() const { return m_tick; }
    f32 tickRate() const { return m_config.tickRate; }
    f64 now() const { return m_now; }

    const std::vector<PeerId>& clients() const { return m_clients; }
    void kick(PeerId client, u32 reason = static_cast<u32>(DisconnectReason::Kicked));
    PeerStats stats(PeerId client) const { return m_transport->stats(client); }
    SnapshotStats lastSnapshotStats(PeerId client) const;

    MessageRegistry& messages() { return m_messages; }
    RpcRegistry& rpcs() { return m_rpcs; }
    ReplicationServer& replication() { return m_replication; }
    ITransport& transport() { return *m_transport; }

    template <NetMessage M>
    void send(PeerId client, const M& msg, Channel channel = Channel::ReliableOrdered) {
        BitWriter w;
        w.writeU8(static_cast<u8>(PacketKind::Message));
        MessageRegistry::encode(w, msg);
        m_transport->send(client, channel, w.data());
    }
    template <NetMessage M>
    void broadcast(const M& msg, Channel channel = Channel::ReliableOrdered) {
        BitWriter w;
        w.writeU8(static_cast<u8>(PacketKind::Message));
        MessageRegistry::encode(w, msg);
        sendToAll(w.data(), channel);
    }
    template <class... Args>
    void callClient(PeerId client, std::string_view rpc, const Args&... args) {
        BitWriter w;
        w.writeU8(static_cast<u8>(PacketKind::Rpc));
        RpcRegistry::encode(w, rpc, args...);
        m_transport->send(client, Channel::ReliableOrdered, w.data());
    }
    template <class... Args>
    void multicast(std::string_view rpc, const Args&... args) {
        BitWriter w;
        w.writeU8(static_cast<u8>(PacketKind::Rpc));
        RpcRegistry::encode(w, rpc, args...);
        sendToAll(w.data(), Channel::ReliableOrdered);
    }
    // Only to clients on which `object` is spawned (relevancy-aware multicast).
    template <class... Args>
    void multicastRelevant(NetId object, std::string_view rpc, const Args&... args) {
        BitWriter w;
        w.writeU8(static_cast<u8>(PacketKind::Rpc));
        RpcRegistry::encode(w, rpc, args...);
        for (PeerId c : m_clients) {
            if (m_replication.isSpawnedOn(c, object)) {
                m_transport->send(c, Channel::ReliableOrdered, w.data());
            }
        }
    }
    void sendToAll(std::span<const u8> packet, Channel channel);

    std::function<void(PeerId)> onClientConnected;
    std::function<void(PeerId, u32 reason)> onClientDisconnected;
    // Packets with kind >= PacketKind::User go here (custom protocols, e.g. input batches).
    std::function<void(PeerId, PacketKind, BitReader&)> onUserPacket;

private:
    void handlePacket(PeerId from, const std::vector<u8>& payload);
    void dropClient(PeerId client, u32 reason);

    std::unique_ptr<ITransport> m_transport;
    NetServerConfig m_config;
    MessageRegistry m_messages;
    RpcRegistry m_rpcs;
    ReplicationServer m_replication;
    std::vector<PeerId> m_clients;
    std::unordered_map<PeerId, SnapshotStats> m_snapshotStats;
    std::vector<TransportEvent> m_events;
    bool m_running = false;
    u32 m_tick = 0;
    f64 m_now = 0.0;
};

struct DedicatedServerConfig {
    NetServerConfig server;
    // Default: ENet. Tests inject a MemoryNetwork transport.
    std::function<std::unique_ptr<ITransport>()> transportFactory;
    u64 maxTicks = 0;                              // 0 = until stopFlag
    const std::atomic<bool>* stopFlag = nullptr;   // set from a signal handler / other thread to stop
    bool realtime = true;                          // false: simulated time, no sleeping (tests, soak runs)
};

using DedicatedTickFn = std::function<void(NetServer& server, u32 tick, f64 dt)>;

// Headless fixed-rate loop: poll -> tickFn (game simulation) -> snapshots. Returns 0 on clean exit.
int runDedicatedServer(const DedicatedServerConfig& config, const DedicatedTickFn& tickFn,
                       const std::function<void(NetServer&)>& onStarted = {});

} // namespace ox::net
