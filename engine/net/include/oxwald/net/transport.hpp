#pragma once

#include <oxwald/core/types.hpp>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ox::net {

// Transport-local handle of a remote peer. 0 is never a valid peer.
using PeerId = u32;
inline constexpr PeerId kInvalidPeer = 0;

enum class Channel : u8 {
    ReliableOrdered = 0,     // retransmitted, delivered in order (chat, RPCs, spawn-critical messages)
    UnreliableSequenced = 1, // may drop, never delivered older-than-latest (inputs)
    Unreliable = 2,          // may drop/reorder (snapshots carry their own sequence numbers)
};
inline constexpr u32 kChannelCount = 3;

// Sent as the 32-bit disconnect payload. ENet reports 0 for timeouts, hence Timeout = 0.
enum class DisconnectReason : u32 {
    Timeout = 0,
    Requested = 1,
    Kicked = 2,
    ServerFull = 3,
    VersionMismatch = 4,
    Shutdown = 5,
    UserBase = 100, // game-defined reasons start here
};

struct PeerStats {
    f32 rttMs = 0.f;
    f32 rttVarianceMs = 0.f;
    f32 packetLoss = 0.f;       // 0..1
    f32 sendBandwidth = 0.f;    // bytes/s (smoothed)
    f32 receiveBandwidth = 0.f; // bytes/s (smoothed)
    u64 bytesSent = 0;
    u64 bytesReceived = 0;
    u64 packetsSent = 0;
    u64 packetsReceived = 0;
};

struct TransportEvent {
    enum class Type : u8 { Connected, Disconnected, Received };
    Type type = Type::Received;
    PeerId peer = kInvalidPeer;
    Channel channel = Channel::ReliableOrdered;
    u32 data = 0; // Connected: connect payload (protocol version); Disconnected: DisconnectReason
    std::vector<u8> payload;
};

struct ListenConfig {
    std::string address = "127.0.0.1"; // "0.0.0.0" for all interfaces
    u16 port = 0;                       // 0 = ephemeral, query localPort()
    u32 maxPeers = 32;
};

// Packet-level transport. Implementations: ENet (UDP), MemoryTransport (in-process, deterministic),
// SimulatedTransport (latency/jitter/loss wrapper around another transport).
// Not thread-safe: drive a transport from one thread.
class ITransport {
public:
    virtual ~ITransport() = default;

    virtual bool listen(const ListenConfig& config) = 0;
    // Starts connecting; a Connected (or Disconnected on failure) event follows from poll().
    virtual PeerId connect(std::string_view host, u16 port, u32 connectData) = 0;
    virtual bool send(PeerId peer, Channel channel, std::span<const u8> payload) = 0;
    virtual void disconnect(PeerId peer, u32 reason) = 0;
    // Pumps the transport (receive, retransmit, flush) and appends events. `now` is the caller's clock in seconds;
    // in-memory transports run on it (deterministic tests), ENet uses its own clock.
    virtual void poll(f64 now, std::vector<TransportEvent>& events) = 0;
    virtual void flush() = 0;
    virtual PeerStats stats(PeerId peer) const = 0;
    virtual u16 localPort() const = 0;
    virtual void close() = 0;
};

struct EnetTransportConfig {
    u32 timeoutMs = 5000;
    u32 incomingBandwidth = 0; // bytes/s, 0 = unlimited
    u32 outgoingBandwidth = 0;
    // Drops this fraction of *incoming* UDP datagrams before ENet sees them (intercept callback), so reliable
    // channels really retransmit. Testing only.
    f32 simulatedIncomingLoss = 0.f;
    u32 lossSeed = 1;
};

std::unique_ptr<ITransport> makeEnetTransport(const EnetTransportConfig& config = {});

} // namespace ox::net
