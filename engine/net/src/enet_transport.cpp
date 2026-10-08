#include "peer_counters.hpp"

#include <oxwald/core/log.hpp>
#include <oxwald/net/transport.hpp>

#include <enet/enet.h>

#include <mutex>
#include <random>
#include <string>
#include <unordered_map>

namespace ox::net {

namespace {

std::mutex g_enetMutex;
int g_enetRefs = 0;

bool acquireEnet() {
    std::lock_guard lock(g_enetMutex);
    if (g_enetRefs == 0 && enet_initialize() != 0) {
        OX_LOG_ERROR("net", "enet_initialize failed");
        return false;
    }
    ++g_enetRefs;
    return true;
}

void releaseEnet() {
    std::lock_guard lock(g_enetMutex);
    if (--g_enetRefs == 0) {
        enet_deinitialize();
    }
}

class EnetTransport;
// ENetHost has no user pointer; the intercept callback only runs inside enet_host_service on the polling thread.
thread_local EnetTransport* t_polling = nullptr;

class EnetTransport final : public ITransport {
public:
    explicit EnetTransport(const EnetTransportConfig& config) : m_config(config), m_rng(config.lossSeed) {
        m_initialized = acquireEnet();
    }

    ~EnetTransport() override {
        close();
        if (m_initialized) {
            releaseEnet();
        }
    }

    bool listen(const ListenConfig& config) override {
        if (!m_initialized || m_host) {
            return false;
        }
        ENetAddress address{};
        address.port = config.port;
        if (config.address.empty() || config.address == "0.0.0.0") {
            address.host = ENET_HOST_ANY;
        } else if (enet_address_set_host_ip(&address, config.address.c_str()) != 0 &&
                   enet_address_set_host(&address, config.address.c_str()) != 0) {
            OX_LOG_ERROR("net", "invalid listen address '{}'", config.address);
            return false;
        }
        m_host = enet_host_create(&address, config.maxPeers, kChannelCount, m_config.incomingBandwidth,
                                  m_config.outgoingBandwidth);
        if (!m_host) {
            OX_LOG_ERROR("net", "failed to listen on {}:{}", config.address, config.port);
            return false;
        }
        installIntercept();
        return true;
    }

    PeerId connect(std::string_view host, u16 port, u32 connectData) override {
        if (!m_initialized) {
            return kInvalidPeer;
        }
        if (!m_host) {
            m_host = enet_host_create(nullptr, 1, kChannelCount, m_config.incomingBandwidth,
                                      m_config.outgoingBandwidth);
            if (!m_host) {
                OX_LOG_ERROR("net", "failed to create client host");
                return kInvalidPeer;
            }
            installIntercept();
        }
        const std::string hostName(host);
        ENetAddress address{};
        address.port = port;
        if (enet_address_set_host_ip(&address, hostName.c_str()) != 0 &&
            enet_address_set_host(&address, hostName.c_str()) != 0) {
            OX_LOG_ERROR("net", "cannot resolve '{}'", hostName);
            return kInvalidPeer;
        }
        ENetPeer* peer = enet_host_connect(m_host, &address, kChannelCount, connectData);
        if (!peer) {
            OX_LOG_ERROR("net", "no free peer slot to connect to {}:{}", hostName, port);
            return kInvalidPeer;
        }
        return registerPeer(peer);
    }

    bool send(PeerId id, Channel channel, std::span<const u8> payload) override {
        Peer* p = find(id);
        if (!p) {
            return false;
        }
        u32 flags = 0;
        switch (channel) {
        case Channel::ReliableOrdered: flags = ENET_PACKET_FLAG_RELIABLE; break;
        case Channel::UnreliableSequenced: flags = 0; break;
        case Channel::Unreliable: flags = ENET_PACKET_FLAG_UNSEQUENCED; break;
        }
        ENetPacket* packet = enet_packet_create(payload.data(), payload.size(), flags);
        if (!packet) {
            return false;
        }
        if (enet_peer_send(p->peer, static_cast<enet_uint8>(channel), packet) != 0) {
            enet_packet_destroy(packet);
            return false;
        }
        p->counters.onSend(payload.size());
        return true;
    }

    void disconnect(PeerId id, u32 reason) override {
        Peer* p = find(id);
        if (!p) {
            return;
        }
        p->localReason = reason;
        p->disconnecting = true;
        enet_peer_disconnect(p->peer, reason);
    }

    void poll(f64 now, std::vector<TransportEvent>& events) override {
        if (!m_host) {
            return;
        }
        t_polling = this;
        ENetEvent ev;
        while (enet_host_service(m_host, &ev, 0) > 0) {
            handle(ev, events);
        }
        t_polling = nullptr;
        for (auto& [id, p] : m_peers) {
            p.counters.update(now);
        }
    }

    void flush() override {
        if (m_host) {
            enet_host_flush(m_host);
        }
    }

    PeerStats stats(PeerId id) const override {
        PeerStats s;
        auto it = m_peers.find(id);
        if (it == m_peers.end()) {
            return s;
        }
        const ENetPeer* peer = it->second.peer;
        s.rttMs = static_cast<f32>(peer->roundTripTime);
        s.rttVarianceMs = static_cast<f32>(peer->roundTripTimeVariance);
        s.packetLoss = static_cast<f32>(peer->packetLoss) / static_cast<f32>(ENET_PEER_PACKET_LOSS_SCALE);
        it->second.counters.fill(s);
        return s;
    }

    u16 localPort() const override {
        if (!m_host) {
            return 0;
        }
        ENetAddress address{};
        if (enet_socket_get_address(m_host->socket, &address) != 0) {
            return m_host->address.port;
        }
        return address.port;
    }

    void close() override {
        if (!m_host) {
            return;
        }
        for (auto& [id, p] : m_peers) {
            p.peer->data = nullptr;
            enet_peer_disconnect_now(p.peer, static_cast<u32>(DisconnectReason::Shutdown));
        }
        m_peers.clear();
        enet_host_flush(m_host);
        enet_host_destroy(m_host);
        m_host = nullptr;
    }

    int intercept(ENetHost*, ENetEvent*) {
        if (m_config.simulatedIncomingLoss <= 0.f) {
            return 0;
        }
        return m_lossDist(m_rng) < m_config.simulatedIncomingLoss ? 1 : 0;
    }

private:
    struct Peer {
        ENetPeer* peer = nullptr;
        detail::PeerCounters counters;
        u32 localReason = 0;
        bool disconnecting = false;
    };

    static int ENET_CALLBACK interceptThunk(ENetHost* host, ENetEvent* event) {
        return t_polling ? t_polling->intercept(host, event) : 0;
    }

    void installIntercept() {
        if (m_config.simulatedIncomingLoss > 0.f) {
            m_host->intercept = &EnetTransport::interceptThunk;
        }
    }

    PeerId registerPeer(ENetPeer* peer) {
        const PeerId id = m_nextId++;
        peer->data = reinterpret_cast<void*>(static_cast<uintptr_t>(id));
        enet_peer_timeout(peer, 0, m_config.timeoutMs, m_config.timeoutMs);
        m_peers[id].peer = peer;
        return id;
    }

    Peer* find(PeerId id) {
        auto it = m_peers.find(id);
        return it == m_peers.end() ? nullptr : &it->second;
    }

    static PeerId idOf(const ENetPeer* peer) {
        return static_cast<PeerId>(reinterpret_cast<uintptr_t>(peer->data));
    }

    void handle(ENetEvent& ev, std::vector<TransportEvent>& events) {
        switch (ev.type) {
        case ENET_EVENT_TYPE_CONNECT: {
            PeerId id = idOf(ev.peer);
            if (id == kInvalidPeer) {
                id = registerPeer(ev.peer); // incoming connection on a listening host
            }
            TransportEvent out;
            out.type = TransportEvent::Type::Connected;
            out.peer = id;
            out.data = ev.data;
            events.push_back(std::move(out));
            break;
        }
        case ENET_EVENT_TYPE_DISCONNECT: {
            const PeerId id = idOf(ev.peer);
            ev.peer->data = nullptr;
            auto it = m_peers.find(id);
            if (it == m_peers.end()) {
                break;
            }
            TransportEvent out;
            out.type = TransportEvent::Type::Disconnected;
            out.peer = id;
            out.data = it->second.disconnecting ? it->second.localReason : ev.data;
            m_peers.erase(it);
            events.push_back(std::move(out));
            break;
        }
        case ENET_EVENT_TYPE_RECEIVE: {
            const PeerId id = idOf(ev.peer);
            if (Peer* p = find(id)) {
                p->counters.onReceive(ev.packet->dataLength);
                TransportEvent out;
                out.type = TransportEvent::Type::Received;
                out.peer = id;
                out.channel = static_cast<Channel>(ev.channelID);
                out.payload.assign(ev.packet->data, ev.packet->data + ev.packet->dataLength);
                events.push_back(std::move(out));
            }
            enet_packet_destroy(ev.packet);
            break;
        }
        default: break;
        }
    }

    EnetTransportConfig m_config;
    bool m_initialized = false;
    ENetHost* m_host = nullptr;
    std::unordered_map<PeerId, Peer> m_peers;
    PeerId m_nextId = 1;
    std::mt19937 m_rng;
    std::uniform_real_distribution<f32> m_lossDist{0.f, 1.f};
};

} // namespace

std::unique_ptr<ITransport> makeEnetTransport(const EnetTransportConfig& config) {
    return std::make_unique<EnetTransport>(config);
}

} // namespace ox::net
