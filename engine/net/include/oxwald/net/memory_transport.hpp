#pragma once

#include <oxwald/net/transport.hpp>

#include <memory>

namespace ox::net {

// Per-datagram network conditions (applied in both directions).
struct LinkConditions {
    f64 latency = 0.0;  // one-way, seconds
    f64 jitter = 0.0;   // extra uniform [0, jitter] seconds per datagram (can reorder)
    f32 loss = 0.f;     // 0..1 probability a datagram is dropped
    f32 duplicate = 0.f; // 0..1 probability a datagram is delivered twice
};

struct MemoryTransportConfig {
    f64 timeout = 5.0;          // seconds without any datagram from the peer
    f64 pingInterval = 0.25;
    f64 connectRetryInterval = 0.2;
    f64 minRetransmitTimeout = 0.05;
};

namespace detail {
struct MemoryNetworkState;
}

// In-process "network": transports created from it exchange datagrams through queues instead of sockets. It runs a
// small reliable-UDP protocol (acks, retransmission, ordering, sequencing) on top, so the same channel semantics
// as ENet hold even with simulated loss. Fully deterministic given a seed and the `now` passed to poll().
// Ports are virtual; connect() ignores the host name.
class MemoryNetwork {
public:
    explicit MemoryNetwork(u32 seed = 1);
    ~MemoryNetwork();

    void setConditions(const LinkConditions& conditions);
    const LinkConditions& conditions() const;
    std::unique_ptr<ITransport> createTransport(const MemoryTransportConfig& config = {});

    u64 datagramsSent() const;
    u64 datagramsDropped() const;

private:
    std::shared_ptr<detail::MemoryNetworkState> m_state;
};

// Wraps any transport and delays/drops *outgoing* packets. Unreliable channels are dropped with `loss`; reliable
// ones are never dropped (the inner transport would retransmit) but a "lost" reliable packet is delayed by an
// extra 2 * latency to model the retransmission. Ordering per (peer, reliable channel) is preserved.
// Wrap both ends to simulate both directions.
class SimulatedTransport final : public ITransport {
public:
    SimulatedTransport(std::unique_ptr<ITransport> inner, LinkConditions conditions, u32 seed = 1);
    ~SimulatedTransport() override;

    void setConditions(const LinkConditions& conditions);
    ITransport& inner() { return *m_inner; }

    bool listen(const ListenConfig& config) override;
    PeerId connect(std::string_view host, u16 port, u32 connectData) override;
    bool send(PeerId peer, Channel channel, std::span<const u8> payload) override;
    void disconnect(PeerId peer, u32 reason) override;
    void poll(f64 now, std::vector<TransportEvent>& events) override;
    void flush() override;
    PeerStats stats(PeerId peer) const override;
    u16 localPort() const override;
    void close() override;

private:
    struct Impl;
    std::unique_ptr<ITransport> m_inner;
    std::unique_ptr<Impl> m_impl;
};

} // namespace ox::net
