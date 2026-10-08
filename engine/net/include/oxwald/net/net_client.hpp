#pragma once

#include <oxwald/net/interpolation.hpp>
#include <oxwald/net/net_server.hpp>

namespace ox::net {

enum class ConnectionState : u8 { Disconnected, Connecting, Connected };

struct NetClientConfig {
    u32 protocolVersion = 1;
    f64 interpolationDelay = 0.1; // ~3 snapshots at 30 Hz
    f64 clockSyncInterval = 0.25;
};

class NetClient {
public:
    NetClient(std::unique_ptr<ITransport> transport, NetClientConfig config = {});
    ~NetClient();

    bool connect(std::string_view host, u16 port);
    void disconnect(u32 reason = static_cast<u32>(DisconnectReason::Requested));
    void poll(f64 now);

    ConnectionState state() const { return m_state; }
    bool connected() const { return m_state == ConnectionState::Connected; }
    PeerId clientId() const { return m_clientId; } // id the server uses for us (NetObject owner)
    f32 serverTickRate() const { return m_serverTickRate; }

    const ClockSync& clock() const { return m_clock; }
    f64 serverTime() const { return m_clock.serverTime(m_now); }
    f64 renderTime() const { return serverTime() - m_config.interpolationDelay; }
    void setInterpolationDelay(f64 seconds) { m_config.interpolationDelay = seconds; }

    PeerStats stats() const { return m_transport->stats(m_server); }
    MessageRegistry& messages() { return m_messages; }
    RpcRegistry& rpcs() { return m_rpcs; }
    ReplicationClient& replication() { return m_replication; }
    ITransport& transport() { return *m_transport; }

    template <NetMessage M>
    void send(const M& msg, Channel channel = Channel::ReliableOrdered) {
        BitWriter w;
        w.writeU8(static_cast<u8>(PacketKind::Message));
        MessageRegistry::encode(w, msg);
        sendRaw(w.data(), channel);
    }
    template <class... Args>
    void callServer(std::string_view rpc, const Args&... args) {
        BitWriter w;
        w.writeU8(static_cast<u8>(PacketKind::Rpc));
        RpcRegistry::encode(w, rpc, args...);
        sendRaw(w.data(), Channel::ReliableOrdered);
    }
    void sendRaw(std::span<const u8> packet, Channel channel);

    std::function<void()> onConnected;
    std::function<void(u32 reason)> onDisconnected;
    std::function<void(PacketKind, BitReader&)> onUserPacket;

private:
    void handlePacket(const std::vector<u8>& payload);
    void sendClockPing();

    std::unique_ptr<ITransport> m_transport;
    NetClientConfig m_config;
    MessageRegistry m_messages;
    RpcRegistry m_rpcs;
    ReplicationClient m_replication;
    ClockSync m_clock;
    ConnectionState m_state = ConnectionState::Disconnected;
    PeerId m_server = kInvalidPeer;
    PeerId m_clientId = kInvalidPeer;
    f32 m_serverTickRate = 0.f;
    f64 m_now = 0.0;
    f64 m_lastPing = -1e9;
    std::vector<TransportEvent> m_events;
};

} // namespace ox::net
