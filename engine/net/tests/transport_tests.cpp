#include "test_harness.hpp"

#include <gtest/gtest.h>

using namespace ox;
using namespace ox::net;
using namespace ox::net::test;

namespace {

std::vector<u8> bytesOf(u32 v) { return {u8(v), u8(v >> 8), u8(v >> 16), u8(v >> 24)}; }
u32 valueOf(const std::vector<u8>& b) { return b[0] | (b[1] << 8) | (b[2] << 16) | (u32(b[3]) << 24); }

struct TransportPair {
    std::unique_ptr<ITransport> server, client;
    PeerId serverSideClient = kInvalidPeer, clientSideServer = kInvalidPeer;
    bool clientConnected = false;
    u32 connectData = 0;
    std::vector<TransportEvent> serverEvents, clientEvents;

    void poll(f64 now) {
        std::vector<TransportEvent> ev;
        server->poll(now, ev);
        for (auto& e : ev) {
            if (e.type == TransportEvent::Type::Connected) {
                serverSideClient = e.peer;
                connectData = e.data;
            }
            serverEvents.push_back(std::move(e));
        }
        ev.clear();
        client->poll(now, ev);
        for (auto& e : ev) {
            if (e.type == TransportEvent::Type::Connected) {
                clientConnected = true;
            }
            clientEvents.push_back(std::move(e));
        }
    }

    std::vector<u32> received(const std::vector<TransportEvent>& events, Channel channel) const {
        std::vector<u32> out;
        for (auto& e : events) {
            if (e.type == TransportEvent::Type::Received && e.channel == channel) {
                out.push_back(valueOf(e.payload));
            }
        }
        return out;
    }
};

void runReliableOrdering(TransportPair& p, const std::function<bool(const std::function<void(f64)>&,
                                                                    const std::function<bool()>&, f64)>& pump) {
    ASSERT_TRUE(p.server->listen({"127.0.0.1", 0, 8}));
    p.clientSideServer = p.client->connect("127.0.0.1", p.server->localPort(), 77);
    ASSERT_NE(p.clientSideServer, kInvalidPeer);
    auto poll = [&](f64 t) { p.poll(t); };
    ASSERT_TRUE(pump(poll, [&] { return p.clientConnected && p.serverSideClient != kInvalidPeer; }, 10.0));
    EXPECT_EQ(p.connectData, 77u);

    constexpr u32 kCount = 200;
    for (u32 i = 0; i < kCount; ++i) {
        p.client->send(p.clientSideServer, Channel::ReliableOrdered, bytesOf(i));
        p.client->send(p.clientSideServer, Channel::Unreliable, bytesOf(1000 + i));
    }
    ASSERT_TRUE(pump(poll, [&] { return p.received(p.serverEvents, Channel::ReliableOrdered).size() >= kCount; },
                     20.0));
    const auto got = p.received(p.serverEvents, Channel::ReliableOrdered);
    ASSERT_EQ(got.size(), kCount);
    for (u32 i = 0; i < kCount; ++i) {
        EXPECT_EQ(got[i], i);
    }
}

} // namespace

TEST(MemoryTransport, ConnectSendDisconnect) {
    MemoryNetwork net;
    TransportPair p{net.createTransport(), net.createTransport()};
    ASSERT_TRUE(p.server->listen({"", 0, 4}));
    p.clientSideServer = p.client->connect("x", p.server->localPort(), 5);
    f64 t = 0;
    for (int i = 0; i < 10; ++i) p.poll(t += 0.01);
    ASSERT_TRUE(p.clientConnected);
    ASSERT_NE(p.serverSideClient, kInvalidPeer);
    EXPECT_EQ(p.connectData, 5u);

    p.server->send(p.serverSideClient, Channel::ReliableOrdered, bytesOf(42));
    for (int i = 0; i < 5; ++i) p.poll(t += 0.01);
    ASSERT_EQ(p.received(p.clientEvents, Channel::ReliableOrdered), std::vector<u32>{42});

    p.server->disconnect(p.serverSideClient, static_cast<u32>(DisconnectReason::Kicked));
    for (int i = 0; i < 5; ++i) p.poll(t += 0.01);
    bool clientSawKick = false;
    for (auto& e : p.clientEvents) {
        if (e.type == TransportEvent::Type::Disconnected) {
            clientSawKick = e.data == static_cast<u32>(DisconnectReason::Kicked);
        }
    }
    EXPECT_TRUE(clientSawKick);
}

TEST(MemoryTransport, TimeoutWhenPeerVanishes) {
    MemoryNetwork net;
    MemoryTransportConfig cfg;
    cfg.timeout = 0.5;
    TransportPair p{net.createTransport(cfg), net.createTransport(cfg)};
    ASSERT_TRUE(p.server->listen({}));
    p.clientSideServer = p.client->connect("x", p.server->localPort(), 0);
    f64 t = 0;
    for (int i = 0; i < 10; ++i) p.poll(t += 0.01);
    ASSERT_TRUE(p.clientConnected);
    net.setConditions({.loss = 1.f}); // cable cut
    for (int i = 0; i < 100; ++i) p.poll(t += 0.01);
    bool timedOut = false;
    for (auto& e : p.serverEvents) {
        timedOut |= e.type == TransportEvent::Type::Disconnected && e.data == u32(DisconnectReason::Timeout);
    }
    EXPECT_TRUE(timedOut);
}

TEST(MemoryTransport, ReliableOrderingUnderLossJitterAndDuplicates) {
    MemoryNetwork net(99);
    net.setConditions({.latency = 0.03, .jitter = 0.02, .loss = 0.25f, .duplicate = 0.1f});
    TransportPair p{net.createTransport(), net.createTransport()};
    f64 t = 0;
    runReliableOrdering(p, [&](const std::function<void(f64)>& poll, const std::function<bool()>& pred, f64 max) {
        const f64 end = t + max;
        while (t < end) {
            if (pred()) return true;
            poll(t += 0.005);
        }
        return pred();
    });
    EXPECT_GT(net.datagramsDropped(), 0u);
    const auto unreliable = p.received(p.serverEvents, Channel::Unreliable);
    EXPECT_LT(unreliable.size(), 200u * 1.1); // some lost (and a few duplicated)
    EXPECT_GT(p.client->stats(p.clientSideServer).rttMs, 40.f);
}

TEST(MemoryTransport, SequencedChannelDropsOlderPackets) {
    MemoryNetwork net(3);
    net.setConditions({.latency = 0.01, .jitter = 0.05});
    TransportPair p{net.createTransport(), net.createTransport()};
    ASSERT_TRUE(p.server->listen({}));
    p.clientSideServer = p.client->connect("x", p.server->localPort(), 0);
    f64 t = 0;
    for (int i = 0; i < 40; ++i) p.poll(t += 0.01);
    ASSERT_TRUE(p.clientConnected);
    for (u32 i = 0; i < 100; ++i) {
        p.client->send(p.clientSideServer, Channel::UnreliableSequenced, bytesOf(i));
        p.poll(t += 0.002);
    }
    for (int i = 0; i < 50; ++i) p.poll(t += 0.01);
    const auto got = p.received(p.serverEvents, Channel::UnreliableSequenced);
    ASSERT_FALSE(got.empty());
    EXPECT_TRUE(std::is_sorted(got.begin(), got.end()));
    EXPECT_LT(got.size(), 100u); // jitter reorders, older ones are dropped
}

TEST(SimulatedTransport, AddsLatencyAndDropsUnreliable) {
    MemoryNetwork net;
    TransportPair p;
    p.server = net.createTransport();
    p.client = std::make_unique<SimulatedTransport>(net.createTransport(),
                                                    LinkConditions{.latency = 0.1, .loss = 0.5f}, 5);
    ASSERT_TRUE(p.server->listen({}));
    p.clientSideServer = p.client->connect("x", p.server->localPort(), 0);
    f64 t = 0;
    for (int i = 0; i < 10; ++i) p.poll(t += 0.01);
    ASSERT_TRUE(p.clientConnected);
    for (u32 i = 0; i < 100; ++i) {
        p.client->send(p.clientSideServer, Channel::ReliableOrdered, bytesOf(i));
        p.client->send(p.clientSideServer, Channel::Unreliable, bytesOf(i));
    }
    p.poll(t += 0.05);
    EXPECT_TRUE(p.received(p.serverEvents, Channel::ReliableOrdered).empty()); // still delayed
    for (int i = 0; i < 100; ++i) p.poll(t += 0.01);
    const auto reliable = p.received(p.serverEvents, Channel::ReliableOrdered);
    ASSERT_EQ(reliable.size(), 100u);
    EXPECT_TRUE(std::is_sorted(reliable.begin(), reliable.end()));
    const auto unreliable = p.received(p.serverEvents, Channel::Unreliable);
    EXPECT_GT(unreliable.size(), 20u);
    EXPECT_LT(unreliable.size(), 80u);
}

TEST(EnetTransport, ConnectDisconnectWithReason) {
    TransportPair p{makeEnetTransport(), makeEnetTransport()};
    ASSERT_TRUE(p.server->listen({"127.0.0.1", 0, 4}));
    ASSERT_NE(p.server->localPort(), 0);
    p.clientSideServer = p.client->connect("127.0.0.1", p.server->localPort(), 3);
    auto poll = [&](f64 t) { p.poll(t); };
    ASSERT_TRUE(pumpReal(poll, [&] { return p.clientConnected && p.serverSideClient != kInvalidPeer; }, 5.0));
    EXPECT_EQ(p.connectData, 3u);

    p.client->send(p.clientSideServer, Channel::ReliableOrdered, bytesOf(7));
    ASSERT_TRUE(pumpReal(poll, [&] { return !p.received(p.serverEvents, Channel::ReliableOrdered).empty(); }, 5.0));
    const PeerStats s = p.server->stats(p.serverSideClient);
    EXPECT_EQ(s.bytesReceived, 4u);
    EXPECT_EQ(p.client->stats(p.clientSideServer).bytesSent, 4u);

    p.client->disconnect(p.clientSideServer, static_cast<u32>(DisconnectReason::UserBase) + 1);
    auto disconnectReason = [&]() -> std::optional<u32> {
        for (auto& e : p.serverEvents) {
            if (e.type == TransportEvent::Type::Disconnected) return e.data;
        }
        return std::nullopt;
    };
    ASSERT_TRUE(pumpReal(poll, [&] { return disconnectReason().has_value(); }, 5.0));
    EXPECT_EQ(*disconnectReason(), static_cast<u32>(DisconnectReason::UserBase) + 1);
}

TEST(EnetTransport, ReliableOrderingUnderRealPacketLoss) {
    EnetTransportConfig lossy;
    lossy.simulatedIncomingLoss = 0.2f;
    lossy.timeoutMs = 10000;
    TransportPair p{makeEnetTransport(lossy), makeEnetTransport(lossy)};
    runReliableOrdering(p, pumpReal);
}
