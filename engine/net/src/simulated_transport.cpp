#include <oxwald/net/memory_transport.hpp>

#include <algorithm>
#include <map>
#include <random>

namespace ox::net {

struct SimulatedTransport::Impl {
    struct Delayed {
        f64 sendAt = 0;
        u64 order = 0;
        PeerId peer = kInvalidPeer;
        Channel channel = Channel::Unreliable;
        std::vector<u8> payload;
    };

    LinkConditions conditions;
    std::mt19937 rng;
    std::uniform_real_distribution<f64> uniform{0.0, 1.0};
    std::vector<Delayed> queue;
    std::map<PeerId, f64> lastReliableSendAt; // keeps reliable channel ordering
    f64 now = 0.0;
    u64 order = 0;
};

SimulatedTransport::SimulatedTransport(std::unique_ptr<ITransport> inner, LinkConditions conditions, u32 seed)
    : m_inner(std::move(inner)), m_impl(std::make_unique<Impl>()) {
    m_impl->conditions = conditions;
    m_impl->rng.seed(seed);
}

SimulatedTransport::~SimulatedTransport() = default;

void SimulatedTransport::setConditions(const LinkConditions& conditions) { m_impl->conditions = conditions; }

bool SimulatedTransport::listen(const ListenConfig& config) { return m_inner->listen(config); }

PeerId SimulatedTransport::connect(std::string_view host, u16 port, u32 connectData) {
    return m_inner->connect(host, port, connectData);
}

bool SimulatedTransport::send(PeerId peer, Channel channel, std::span<const u8> payload) {
    Impl& s = *m_impl;
    const LinkConditions& c = s.conditions;
    const bool lost = c.loss > 0.f && s.uniform(s.rng) < c.loss;
    f64 delay = c.latency + c.jitter * s.uniform(s.rng);
    if (channel == Channel::ReliableOrdered) {
        if (lost) {
            delay += 2.0 * c.latency + 0.001;
        }
        f64& last = s.lastReliableSendAt[peer];
        const f64 at = std::max(s.now + delay, last);
        last = at;
        delay = at - s.now;
    } else if (lost) {
        return true; // dropped silently, like the wire would
    }
    if (delay <= 0.0) {
        return m_inner->send(peer, channel, payload);
    }
    s.queue.push_back({s.now + delay, s.order++, peer, channel, {payload.begin(), payload.end()}});
    return true;
}

void SimulatedTransport::disconnect(PeerId peer, u32 reason) {
    std::erase_if(m_impl->queue, [peer](const Impl::Delayed& d) { return d.peer == peer; });
    m_impl->lastReliableSendAt.erase(peer);
    m_inner->disconnect(peer, reason);
}

void SimulatedTransport::poll(f64 now, std::vector<TransportEvent>& events) {
    Impl& s = *m_impl;
    s.now = now;
    std::sort(s.queue.begin(), s.queue.end(), [](const Impl::Delayed& a, const Impl::Delayed& b) {
        return a.sendAt != b.sendAt ? a.sendAt < b.sendAt : a.order < b.order;
    });
    usize sent = 0;
    for (; sent < s.queue.size() && s.queue[sent].sendAt <= now; ++sent) {
        m_inner->send(s.queue[sent].peer, s.queue[sent].channel, s.queue[sent].payload);
    }
    s.queue.erase(s.queue.begin(), s.queue.begin() + static_cast<std::ptrdiff_t>(sent));
    m_inner->poll(now, events);
}

void SimulatedTransport::flush() { m_inner->flush(); }

PeerStats SimulatedTransport::stats(PeerId peer) const {
    PeerStats s = m_inner->stats(peer);
    s.rttMs += static_cast<f32>(m_impl->conditions.latency * 1000.0);
    return s;
}

u16 SimulatedTransport::localPort() const { return m_inner->localPort(); }

void SimulatedTransport::close() {
    m_impl->queue.clear();
    m_inner->close();
}

} // namespace ox::net
