#include <oxwald/core/log.hpp>
#include <oxwald/net/net_client.hpp>
#include <oxwald/net/net_server.hpp>

#include <chrono>
#include <thread>

namespace ox::net {

// ---------------------------------------------------------------- NetServer

NetServer::NetServer(std::unique_ptr<ITransport> transport, NetServerConfig config)
    : m_transport(std::move(transport)), m_config(std::move(config)), m_replication(m_config.replication) {}

NetServer::~NetServer() { stop(); }

bool NetServer::start() {
    ListenConfig listen = m_config.listen;
    listen.maxPeers = std::max(listen.maxPeers, m_config.maxClients + 1); // +1 slot to send "server full"
    m_running = m_transport->listen(listen);
    if (m_running) {
        OX_LOG_INFO("net", "server listening on port {} ({} Hz)", m_transport->localPort(), m_config.tickRate);
    }
    return m_running;
}

void NetServer::stop() {
    if (!m_running) {
        return;
    }
    for (PeerId c : std::vector<PeerId>(m_clients)) {
        m_transport->disconnect(c, static_cast<u32>(DisconnectReason::Shutdown));
        dropClient(c, static_cast<u32>(DisconnectReason::Shutdown));
    }
    m_transport->flush();
    m_transport->close();
    m_running = false;
}

void NetServer::kick(PeerId client, u32 reason) {
    m_transport->disconnect(client, reason);
}

SnapshotStats NetServer::lastSnapshotStats(PeerId client) const {
    auto it = m_snapshotStats.find(client);
    return it == m_snapshotStats.end() ? SnapshotStats{} : it->second;
}

void NetServer::sendToAll(std::span<const u8> packet, Channel channel) {
    for (PeerId c : m_clients) {
        m_transport->send(c, channel, packet);
    }
}

void NetServer::dropClient(PeerId client, u32 reason) {
    auto it = std::find(m_clients.begin(), m_clients.end(), client);
    if (it == m_clients.end()) {
        return;
    }
    m_clients.erase(it);
    m_replication.removeClient(client);
    m_snapshotStats.erase(client);
    OX_LOG_INFO("net", "client {} disconnected (reason {})", client, reason);
    if (onClientDisconnected) {
        onClientDisconnected(client, reason);
    }
}

void NetServer::poll(f64 now) {
    m_now = now;
    if (!m_running) {
        return;
    }
    m_events.clear();
    m_transport->poll(now, m_events);
    for (TransportEvent& e : m_events) {
        switch (e.type) {
        case TransportEvent::Type::Connected: {
            if (e.data != m_config.protocolVersion) {
                OX_LOG_WARN("net", "rejecting peer {}: protocol {} != {}", e.peer, e.data, m_config.protocolVersion);
                m_transport->disconnect(e.peer, static_cast<u32>(DisconnectReason::VersionMismatch));
                break;
            }
            if (m_clients.size() >= m_config.maxClients) {
                m_transport->disconnect(e.peer, static_cast<u32>(DisconnectReason::ServerFull));
                break;
            }
            m_clients.push_back(e.peer);
            m_replication.addClient(e.peer);
            BitWriter w;
            w.writeU8(static_cast<u8>(PacketKind::Welcome));
            w.writeVarU32(e.peer);
            w.writeF32(m_config.tickRate);
            w.writeF64(m_now);
            m_transport->send(e.peer, Channel::ReliableOrdered, w.data());
            OX_LOG_INFO("net", "client {} connected", e.peer);
            if (onClientConnected) {
                onClientConnected(e.peer);
            }
            break;
        }
        case TransportEvent::Type::Disconnected: dropClient(e.peer, e.data); break;
        case TransportEvent::Type::Received: handlePacket(e.peer, e.payload); break;
        }
    }
    m_transport->flush();
}

void NetServer::handlePacket(PeerId from, const std::vector<u8>& payload) {
    if (std::find(m_clients.begin(), m_clients.end(), from) == m_clients.end()) {
        return;
    }
    BitReader r(payload);
    const auto kind = static_cast<PacketKind>(r.readU8());
    switch (kind) {
    case PacketKind::Message: {
        const u32 id = r.readU32();
        m_messages.dispatch(from, id, r);
        break;
    }
    case PacketKind::Rpc: {
        const u32 id = r.readU32();
        m_rpcs.dispatch(from, id, r);
        break;
    }
    case PacketKind::SnapshotAck: {
        const u32 latest = r.readVarU32();
        const u32 bits = r.readU32();
        if (r.ok()) {
            m_replication.processAck(from, latest, bits);
        }
        break;
    }
    case PacketKind::ClockPing: {
        const f64 clientTime = r.readF64();
        if (!r.ok()) {
            break;
        }
        BitWriter w;
        w.writeU8(static_cast<u8>(PacketKind::ClockPong));
        w.writeF64(clientTime);
        w.writeF64(m_now);
        m_transport->send(from, Channel::Unreliable, w.data());
        break;
    }
    default:
        if (static_cast<u8>(kind) >= static_cast<u8>(PacketKind::User) && onUserPacket) {
            onUserPacket(from, kind, r);
        } else {
            OX_LOG_WARN("net", "unexpected packet kind {} from client {}", static_cast<u32>(kind), from);
        }
        break;
    }
}

void NetServer::tick(f64 now) {
    m_now = now;
    if (!m_running) {
        return;
    }
    ++m_tick;
    m_replication.beginTick();
    for (PeerId c : m_clients) {
        BitWriter w;
        w.writeU8(static_cast<u8>(PacketKind::Snapshot));
        m_snapshotStats[c] = m_replication.writeSnapshot(c, w, m_tick, now);
        m_transport->send(c, Channel::Unreliable, w.data());
    }
    m_transport->flush();
}

// ---------------------------------------------------------------- NetClient

NetClient::NetClient(std::unique_ptr<ITransport> transport, NetClientConfig config)
    : m_transport(std::move(transport)), m_config(config) {}

NetClient::~NetClient() {
    if (m_state != ConnectionState::Disconnected) {
        m_transport->disconnect(m_server, static_cast<u32>(DisconnectReason::Requested));
        m_transport->flush();
    }
}

bool NetClient::connect(std::string_view host, u16 port) {
    if (m_state != ConnectionState::Disconnected) {
        return false;
    }
    m_server = m_transport->connect(host, port, m_config.protocolVersion);
    if (m_server == kInvalidPeer) {
        return false;
    }
    m_state = ConnectionState::Connecting;
    return true;
}

void NetClient::disconnect(u32 reason) {
    if (m_state == ConnectionState::Disconnected) {
        return;
    }
    m_transport->disconnect(m_server, reason);
    m_transport->flush();
}

void NetClient::sendRaw(std::span<const u8> packet, Channel channel) {
    if (m_state == ConnectionState::Connected) {
        m_transport->send(m_server, channel, packet);
    }
}

void NetClient::sendClockPing() {
    BitWriter w;
    w.writeU8(static_cast<u8>(PacketKind::ClockPing));
    w.writeF64(m_now);
    m_transport->send(m_server, Channel::Unreliable, w.data());
    m_lastPing = m_now;
}

void NetClient::poll(f64 now) {
    m_now = now;
    m_events.clear();
    m_transport->poll(now, m_events);
    for (TransportEvent& e : m_events) {
        if (e.peer != m_server) {
            continue;
        }
        switch (e.type) {
        case TransportEvent::Type::Connected: break; // wait for Welcome
        case TransportEvent::Type::Disconnected: {
            const bool wasConnected = m_state != ConnectionState::Disconnected;
            m_state = ConnectionState::Disconnected;
            m_server = kInvalidPeer;
            m_replication.clear();
            if (wasConnected && onDisconnected) {
                onDisconnected(e.data);
            }
            break;
        }
        case TransportEvent::Type::Received: handlePacket(e.payload); break;
        }
    }
    if (m_state == ConnectionState::Connected) {
        if (now - m_lastPing >= m_config.clockSyncInterval) {
            sendClockPing();
        }
        if (m_replication.hasAckToSend()) {
            BitWriter w;
            w.writeU8(static_cast<u8>(PacketKind::SnapshotAck));
            m_replication.writeAck(w);
            m_transport->send(m_server, Channel::Unreliable, w.data());
            m_replication.clearAckDirty();
        }
    }
    m_transport->flush();
}

void NetClient::handlePacket(const std::vector<u8>& payload) {
    BitReader r(payload);
    const auto kind = static_cast<PacketKind>(r.readU8());
    switch (kind) {
    case PacketKind::Welcome: {
        m_clientId = r.readVarU32();
        m_serverTickRate = r.readF32();
        r.readF64();
        if (!r.ok()) {
            return;
        }
        m_replication.setLocalPeer(m_clientId);
        m_state = ConnectionState::Connected;
        sendClockPing();
        OX_LOG_INFO("net", "connected as client {}", m_clientId);
        if (onConnected) {
            onConnected();
        }
        break;
    }
    case PacketKind::Message: {
        const u32 id = r.readU32();
        m_messages.dispatch(m_server, id, r);
        break;
    }
    case PacketKind::Rpc: {
        const u32 id = r.readU32();
        m_rpcs.dispatch(m_server, id, r);
        break;
    }
    case PacketKind::Snapshot: m_replication.readSnapshot(r); break;
    case PacketKind::ClockPong: {
        const f64 sent = r.readF64();
        const f64 serverTime = r.readF64();
        if (r.ok()) {
            m_clock.addSample(sent, serverTime, m_now);
        }
        break;
    }
    default:
        if (static_cast<u8>(kind) >= static_cast<u8>(PacketKind::User) && onUserPacket) {
            onUserPacket(kind, r);
        }
        break;
    }
}

// ---------------------------------------------------------------- dedicated server

int runDedicatedServer(const DedicatedServerConfig& config, const DedicatedTickFn& tickFn,
                       const std::function<void(NetServer&)>& onStarted) {
    auto transport = config.transportFactory ? config.transportFactory() : makeEnetTransport();
    NetServer server(std::move(transport), config.server);
    if (!server.start()) {
        OX_LOG_ERROR("net", "dedicated server failed to start");
        return 1;
    }
    if (onStarted) {
        onStarted(server);
    }
    using Clock = std::chrono::steady_clock;
    const f64 dt = 1.0 / std::max(1.f, config.server.tickRate);
    const auto start = Clock::now();
    u64 ticks = 0;
    while (!(config.stopFlag && config.stopFlag->load(std::memory_order_relaxed)) &&
           (config.maxTicks == 0 || ticks < config.maxTicks)) {
        const f64 now = config.realtime ? std::chrono::duration<f64>(Clock::now() - start).count()
                                        : static_cast<f64>(ticks) * dt;
        server.poll(now);
        if (tickFn) {
            tickFn(server, server.currentTick() + 1, dt);
        }
        server.tick(now);
        ++ticks;
        if (config.realtime) {
            std::this_thread::sleep_until(start + std::chrono::duration_cast<Clock::duration>(
                                                      std::chrono::duration<f64>(static_cast<f64>(ticks) * dt)));
        }
    }
    server.stop();
    OX_LOG_INFO("net", "dedicated server stopped after {} ticks", ticks);
    return 0;
}

} // namespace ox::net
