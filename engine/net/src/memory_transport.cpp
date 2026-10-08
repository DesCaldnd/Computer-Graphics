#include "peer_counters.hpp"

#include <oxwald/core/log.hpp>
#include <oxwald/net/bit_stream.hpp>
#include <oxwald/net/memory_transport.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <random>
#include <unordered_map>

namespace ox::net {

namespace detail {

struct Datagram {
    u16 from = 0;
    f64 deliverAt = 0.0;
    u64 order = 0; // tie-breaker keeps FIFO for equal delivery times
    std::vector<u8> bytes;
};

struct MemoryNetworkState {
    explicit MemoryNetworkState(u32 seed) : rng(seed) {}

    LinkConditions conditions;
    std::mt19937 rng;
    std::uniform_real_distribution<f64> uniform{0.0, 1.0};
    std::unordered_map<u16, std::vector<Datagram>> queues; // destination port -> in-flight datagrams
    u16 nextEphemeral = 49152;
    u64 order = 0;
    u64 sent = 0, dropped = 0;

    u16 bind(u16 port) {
        if (port == 0) {
            while (queues.contains(nextEphemeral)) {
                ++nextEphemeral;
            }
            port = nextEphemeral++;
        }
        if (queues.contains(port)) {
            return 0;
        }
        queues[port];
        return port;
    }

    void unbind(u16 port) { queues.erase(port); }

    void post(u16 from, u16 to, std::vector<u8> bytes, f64 now) {
        ++sent;
        auto it = queues.find(to);
        if (it == queues.end()) {
            return;
        }
        if (conditions.loss > 0.f && uniform(rng) < conditions.loss) {
            ++dropped;
            return;
        }
        const int copies = (conditions.duplicate > 0.f && uniform(rng) < conditions.duplicate) ? 2 : 1;
        for (int i = 0; i < copies; ++i) {
            Datagram d;
            d.from = from;
            d.deliverAt = now + conditions.latency + conditions.jitter * uniform(rng);
            d.order = order++;
            d.bytes = (i + 1 == copies) ? std::move(bytes) : bytes;
            it->second.push_back(std::move(d));
        }
    }

    std::vector<Datagram> receive(u16 port, f64 now) {
        std::vector<Datagram> out;
        auto it = queues.find(port);
        if (it == queues.end()) {
            return out;
        }
        auto& q = it->second;
        auto split = std::partition(q.begin(), q.end(), [now](const Datagram& d) { return d.deliverAt > now; });
        out.assign(std::make_move_iterator(split), std::make_move_iterator(q.end()));
        q.erase(split, q.end());
        std::sort(out.begin(), out.end(), [](const Datagram& a, const Datagram& b) {
            return a.deliverAt != b.deliverAt ? a.deliverAt < b.deliverAt : a.order < b.order;
        });
        return out;
    }
};

} // namespace detail

namespace {

enum class Kind : u8 { ConnectReq = 1, ConnectAck, Disconnect, Data, Ack, Ping, Pong };

class MemoryTransport final : public ITransport {
public:
    MemoryTransport(std::shared_ptr<detail::MemoryNetworkState> net, const MemoryTransportConfig& config)
        : m_net(std::move(net)), m_config(config) {}

    ~MemoryTransport() override { close(); }

    bool listen(const ListenConfig& config) override {
        if (m_port != 0) {
            return false;
        }
        m_port = m_net->bind(config.port);
        m_listening = m_port != 0;
        m_maxPeers = config.maxPeers;
        return m_listening;
    }

    PeerId connect(std::string_view, u16 port, u32 connectData) override {
        if (m_port == 0) {
            m_port = m_net->bind(0);
        }
        Conn& c = createConn(port);
        c.state = Conn::State::Connecting;
        c.connectData = connectData;
        c.connectStart = m_now;
        sendConnectReq(c);
        return c.id;
    }

    bool send(PeerId id, Channel channel, std::span<const u8> payload) override {
        Conn* c = find(id);
        if (!c) {
            return false;
        }
        u32 seq = 0;
        if (channel == Channel::ReliableOrdered) {
            seq = c->reliableSendSeq++;
            Pending& p = c->unacked[seq];
            p.payload.assign(payload.begin(), payload.end());
            p.firstSend = p.lastSend = m_now;
            p.sends = 1;
            ++c->reliableSends;
        } else if (channel == Channel::UnreliableSequenced) {
            seq = ++c->sequencedSendSeq;
        }
        c->counters.onSend(payload.size());
        // Connecting peers queue reliable data (resent once connected); unreliable data is dropped.
        if (c->state == Conn::State::Connected) {
            sendData(*c, channel, seq, payload);
        }
        return true;
    }

    void disconnect(PeerId id, u32 reason) override {
        auto it = m_conns.find(id);
        if (it == m_conns.end()) {
            return;
        }
        BitWriter w;
        w.writeU8(static_cast<u8>(Kind::Disconnect));
        w.writeU32(reason);
        // No handshake for disconnects: send a few copies so the peer most likely hears it, else it times out.
        for (int i = 0; i < 3; ++i) {
            post(it->second.remotePort, w.data());
        }
        m_localEvents.push_back(makeDisconnect(id, reason));
        m_byPort.erase(it->second.remotePort);
        m_conns.erase(it);
    }

    void poll(f64 now, std::vector<TransportEvent>& events) override {
        m_now = now;
        for (auto& e : m_localEvents) {
            events.push_back(std::move(e));
        }
        m_localEvents.clear();
        if (m_port == 0) {
            return;
        }
        for (auto& d : m_net->receive(m_port, now)) {
            handleDatagram(d, events);
        }
        std::vector<PeerId> dead;
        for (auto& [id, c] : m_conns) {
            if (!service(c)) {
                dead.push_back(id);
            }
            c.counters.update(now);
        }
        for (PeerId id : dead) {
            events.push_back(makeDisconnect(id, static_cast<u32>(DisconnectReason::Timeout)));
            m_byPort.erase(m_conns[id].remotePort);
            m_conns.erase(id);
        }
    }

    void flush() override {}

    PeerStats stats(PeerId id) const override {
        PeerStats s;
        auto it = m_conns.find(id);
        if (it == m_conns.end()) {
            return s;
        }
        const Conn& c = it->second;
        s.rttMs = static_cast<f32>(c.srtt * 1000.0);
        s.rttVarianceMs = static_cast<f32>(c.rttVar * 1000.0);
        s.packetLoss = c.reliableSends == 0 ? 0.f
                                            : static_cast<f32>(c.retransmits) /
                                                  static_cast<f32>(c.reliableSends + c.retransmits);
        c.counters.fill(s);
        return s;
    }

    u16 localPort() const override { return m_port; }

    void close() override {
        if (m_port == 0) {
            return;
        }
        std::vector<PeerId> ids;
        for (auto& [id, c] : m_conns) {
            ids.push_back(id);
        }
        for (PeerId id : ids) {
            disconnect(id, static_cast<u32>(DisconnectReason::Shutdown));
        }
        m_net->unbind(m_port);
        m_port = 0;
        m_listening = false;
    }

private:
    struct Pending {
        std::vector<u8> payload;
        f64 firstSend = 0, lastSend = 0;
        u32 sends = 0;
    };

    struct Conn {
        enum class State { Connecting, Connected };
        PeerId id = kInvalidPeer;
        u16 remotePort = 0;
        State state = State::Connecting;
        u32 connectData = 0;
        f64 connectStart = 0, lastConnectSend = 0, lastReceive = 0, lastPing = 0;
        u32 reliableSendSeq = 0, reliableRecvNext = 0;
        std::map<u32, Pending> unacked;
        std::map<u32, std::vector<u8>> outOfOrder;
        u32 sequencedSendSeq = 0, sequencedRecvLast = 0;
        f64 srtt = 0.1, rttVar = 0.05;
        bool rttInit = false;
        u64 reliableSends = 0, retransmits = 0;
        detail::PeerCounters counters;
    };

    Conn& createConn(u16 remotePort) {
        const PeerId id = m_nextId++;
        Conn& c = m_conns[id];
        c.id = id;
        c.remotePort = remotePort;
        c.lastReceive = m_now;
        c.lastPing = m_now;
        m_byPort[remotePort] = id;
        return c;
    }

    Conn* find(PeerId id) {
        auto it = m_conns.find(id);
        return it == m_conns.end() ? nullptr : &it->second;
    }

    static TransportEvent makeDisconnect(PeerId id, u32 reason) {
        TransportEvent e;
        e.type = TransportEvent::Type::Disconnected;
        e.peer = id;
        e.data = reason;
        return e;
    }

    void post(u16 to, std::span<const u8> bytes) {
        m_net->post(m_port, to, std::vector<u8>(bytes.begin(), bytes.end()), m_now);
    }

    void sendConnectReq(Conn& c) {
        BitWriter w;
        w.writeU8(static_cast<u8>(Kind::ConnectReq));
        w.writeU32(c.connectData);
        post(c.remotePort, w.data());
        c.lastConnectSend = m_now;
    }

    void sendSimple(u16 to, Kind kind) {
        const u8 b = static_cast<u8>(kind);
        post(to, {&b, 1});
    }

    void sendData(Conn& c, Channel channel, u32 seq, std::span<const u8> payload) {
        BitWriter w;
        w.writeU8(static_cast<u8>(Kind::Data));
        w.writeU8(static_cast<u8>(channel));
        w.writeVarU32(seq);
        w.writeBytes(payload);
        post(c.remotePort, w.data());
    }

    void updateRtt(Conn& c, f64 sample) {
        if (!c.rttInit) {
            c.srtt = sample;
            c.rttVar = sample * 0.5;
            c.rttInit = true;
            return;
        }
        c.rttVar = 0.75 * c.rttVar + 0.25 * std::abs(c.srtt - sample);
        c.srtt = 0.875 * c.srtt + 0.125 * sample;
    }

    void markConnected(Conn& c, std::vector<TransportEvent>& events) {
        if (c.state == Conn::State::Connected) {
            return;
        }
        c.state = Conn::State::Connected;
        updateRtt(c, m_now - c.connectStart);
        TransportEvent e;
        e.type = TransportEvent::Type::Connected;
        e.peer = c.id;
        events.push_back(std::move(e));
        for (auto& [seq, p] : c.unacked) {
            sendData(c, Channel::ReliableOrdered, seq, p.payload);
            p.lastSend = m_now;
        }
    }

    void deliver(Conn& c, Channel channel, std::vector<u8> payload, std::vector<TransportEvent>& events) {
        c.counters.onReceive(payload.size());
        TransportEvent e;
        e.type = TransportEvent::Type::Received;
        e.peer = c.id;
        e.channel = channel;
        e.payload = std::move(payload);
        events.push_back(std::move(e));
    }

    void handleDatagram(detail::Datagram& d, std::vector<TransportEvent>& events) {
        BitReader r(d.bytes);
        const Kind kind = static_cast<Kind>(r.readU8());
        auto byPort = m_byPort.find(d.from);
        Conn* c = byPort == m_byPort.end() ? nullptr : find(byPort->second);

        if (kind == Kind::ConnectReq) {
            const u32 data = r.readU32();
            if (!m_listening) {
                return;
            }
            if (!c) {
                if (m_conns.size() >= m_maxPeers) {
                    BitWriter w;
                    w.writeU8(static_cast<u8>(Kind::Disconnect));
                    w.writeU32(static_cast<u32>(DisconnectReason::ServerFull));
                    post(d.from, w.data());
                    return;
                }
                c = &createConn(d.from);
                c->state = Conn::State::Connected;
                TransportEvent e;
                e.type = TransportEvent::Type::Connected;
                e.peer = c->id;
                e.data = data;
                events.push_back(std::move(e));
            }
            c->lastReceive = m_now;
            sendSimple(d.from, Kind::ConnectAck);
            return;
        }
        if (!c) {
            return;
        }
        c->lastReceive = m_now;
        switch (kind) {
        case Kind::ConnectAck: markConnected(*c, events); break;
        case Kind::Disconnect: {
            const u32 reason = r.readU32();
            events.push_back(makeDisconnect(c->id, reason));
            m_byPort.erase(c->remotePort);
            m_conns.erase(c->id);
            break;
        }
        case Kind::Data: {
            markConnected(*c, events); // data before ConnectAck means the ack was lost
            const auto channel = static_cast<Channel>(r.readU8());
            const u32 seq = r.readVarU32();
            if (!r.ok()) {
                return;
            }
            r.alignToByte();
            const usize offset = r.bitPosition() / 8;
            std::vector<u8> payload(d.bytes.begin() + static_cast<std::ptrdiff_t>(offset), d.bytes.end());
            if (channel == Channel::ReliableOrdered) {
                BitWriter ack;
                ack.writeU8(static_cast<u8>(Kind::Ack));
                ack.writeVarU32(seq);
                post(c->remotePort, ack.data());
                if (seq < c->reliableRecvNext || c->outOfOrder.contains(seq)) {
                    return; // duplicate
                }
                c->outOfOrder.emplace(seq, std::move(payload));
                for (auto it = c->outOfOrder.find(c->reliableRecvNext); it != c->outOfOrder.end();
                     it = c->outOfOrder.find(c->reliableRecvNext)) {
                    deliver(*c, channel, std::move(it->second), events);
                    c->outOfOrder.erase(it);
                    ++c->reliableRecvNext;
                }
            } else if (channel == Channel::UnreliableSequenced) {
                if (seq <= c->sequencedRecvLast) {
                    return;
                }
                c->sequencedRecvLast = seq;
                deliver(*c, channel, std::move(payload), events);
            } else {
                deliver(*c, channel, std::move(payload), events);
            }
            break;
        }
        case Kind::Ack: {
            const u32 seq = r.readVarU32();
            auto it = c->unacked.find(seq);
            if (it != c->unacked.end()) {
                if (it->second.sends == 1) {
                    updateRtt(*c, m_now - it->second.firstSend);
                }
                c->unacked.erase(it);
            }
            break;
        }
        case Kind::Ping: {
            const f64 t = r.readF64();
            BitWriter w;
            w.writeU8(static_cast<u8>(Kind::Pong));
            w.writeF64(t);
            post(c->remotePort, w.data());
            break;
        }
        case Kind::Pong: updateRtt(*c, m_now - r.readF64()); break;
        default: break;
        }
    }

    // Returns false when the connection timed out.
    bool service(Conn& c) {
        if (m_now - c.lastReceive > m_config.timeout) {
            return false;
        }
        if (c.state == Conn::State::Connecting) {
            if (m_now - c.lastConnectSend >= m_config.connectRetryInterval) {
                sendConnectReq(c);
            }
            return true;
        }
        const f64 rto = std::clamp(c.srtt + 4.0 * c.rttVar, m_config.minRetransmitTimeout, 1.0);
        for (auto& [seq, p] : c.unacked) {
            if (m_now - p.lastSend >= rto) {
                sendData(c, Channel::ReliableOrdered, seq, p.payload);
                p.lastSend = m_now;
                ++p.sends;
                ++c.retransmits;
            }
        }
        if (m_now - c.lastPing >= m_config.pingInterval) {
            BitWriter w;
            w.writeU8(static_cast<u8>(Kind::Ping));
            w.writeF64(m_now);
            post(c.remotePort, w.data());
            c.lastPing = m_now;
        }
        return true;
    }

    std::shared_ptr<detail::MemoryNetworkState> m_net;
    MemoryTransportConfig m_config;
    u16 m_port = 0;
    bool m_listening = false;
    u32 m_maxPeers = 32;
    f64 m_now = 0.0;
    PeerId m_nextId = 1;
    std::unordered_map<PeerId, Conn> m_conns;
    std::unordered_map<u16, PeerId> m_byPort;
    std::vector<TransportEvent> m_localEvents;
};

} // namespace

MemoryNetwork::MemoryNetwork(u32 seed) : m_state(std::make_shared<detail::MemoryNetworkState>(seed)) {}
MemoryNetwork::~MemoryNetwork() = default;

void MemoryNetwork::setConditions(const LinkConditions& conditions) { m_state->conditions = conditions; }
const LinkConditions& MemoryNetwork::conditions() const { return m_state->conditions; }

std::unique_ptr<ITransport> MemoryNetwork::createTransport(const MemoryTransportConfig& config) {
    return std::make_unique<MemoryTransport>(m_state, config);
}

u64 MemoryNetwork::datagramsSent() const { return m_state->sent; }
u64 MemoryNetwork::datagramsDropped() const { return m_state->dropped; }

} // namespace ox::net
