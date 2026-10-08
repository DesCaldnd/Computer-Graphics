#include "test_harness.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <map>
#include <unordered_map>
#include <random>

using namespace ox;
using namespace ox::net;
using namespace ox::net::test;

namespace {

struct TestObject : NetObject {
    f32 f[8] = {};
    glm::vec3 pos{0.f};
    std::string label;
    int applied = 0;
    InterpolationBuffer interp;

    TestObject() : NetObject("TestObject") {
        for (int i = 0; i < 8; ++i) {
            replicate("f" + std::to_string(i), f[i]);
        }
        replicate("pos", pos, QuantizedVec3Codec{-1000.f, 1000.f, 0.01f});
        replicate("label", label);
        setPositionSource([this] { return pos; });
    }
    void onSnapshotApplied(f64 serverTime) override {
        ++applied;
        interp.push({.time = serverTime, .position = pos});
    }
};

PeerId onlyClient(NetServer& s) { return s.clients().empty() ? kInvalidPeer : s.clients().front(); }

TestObject* clientObject(NetClient& c, NetId id) { return static_cast<TestObject*>(c.replication().find(id)); }

void stepToNextTick(MemoryHarness& h) {
    const u32 tick = h.server.currentTick();
    while (h.server.currentTick() == tick) {
        h.step();
    }
}

} // namespace

TEST(NetSession, ConnectDisconnectInProcess) {
    MemoryHarness h;
    int connected = 0, disconnected = 0;
    h.server.onClientConnected = [&](PeerId) { ++connected; };
    h.server.onClientDisconnected = [&](PeerId, u32 reason) {
        ++disconnected;
        EXPECT_EQ(reason, u32(DisconnectReason::Requested));
    };
    ASSERT_TRUE(h.startAndConnect());
    EXPECT_EQ(connected, 1);
    EXPECT_EQ(h.client.clientId(), onlyClient(h.server));
    EXPECT_EQ(h.client.serverTickRate(), 30.f);
    h.client.disconnect();
    ASSERT_TRUE(h.runUntil([&] { return disconnected == 1; }, 2.0));
    EXPECT_EQ(h.client.state(), ConnectionState::Disconnected);
    EXPECT_TRUE(h.server.clients().empty());
}

TEST(NetSession, VersionMismatchIsRejected) {
    NetClientConfig cc;
    cc.protocolVersion = 2;
    MemoryHarness h({}, cc);
    u32 reason = 999;
    h.client.onDisconnected = [&](u32 r) { reason = r; };
    ASSERT_TRUE(h.server.start());
    ASSERT_TRUE(h.client.connect("m", h.server.port()));
    ASSERT_TRUE(h.runUntil([&] { return reason != 999; }, 2.0));
    EXPECT_EQ(reason, u32(DisconnectReason::VersionMismatch));
    EXPECT_FALSE(h.client.connected());
}

TEST(NetSession, EnetServerAndClientInProcess) {
    NetServer server(makeEnetTransport());
    NetClient client(makeEnetTransport());
    ASSERT_TRUE(server.start());
    ASSERT_TRUE(client.connect("127.0.0.1", server.port()));
    auto poll = [&](f64 t) {
        server.poll(t);
        client.poll(t);
    };
    ASSERT_TRUE(pumpReal(poll, [&] { return client.connected() && server.clients().size() == 1; }, 5.0));
    bool gone = false;
    server.onClientDisconnected = [&](PeerId, u32) { gone = true; };
    client.disconnect();
    ASSERT_TRUE(pumpReal(poll, [&] { return gone && !client.connected(); }, 5.0));
}

struct Chat {
    static constexpr std::string_view kNetName = "test.chat";
    std::string text;
    u8 channel = 0;
    void serialize(BitWriter& w) const {
        w.writeString(text);
        w.writeU8(channel);
    }
    void deserialize(BitReader& r) {
        text = r.readString(256);
        channel = r.readU8();
    }
};

TEST(NetSession, MessagesAndRpcs) {
    MemoryHarness h;
    ASSERT_TRUE(h.startAndConnect());
    std::vector<std::string> serverChat, clientChat;
    f32 hitDamage = 0;
    glm::vec3 hitAt{0.f};
    PeerId hitFrom = kInvalidPeer;
    std::string announced;
    h.server.messages().on<Chat>([&](PeerId from, const Chat& c) {
        serverChat.push_back(c.text);
        h.server.broadcast(Chat{"echo:" + c.text, c.channel});
    });
    h.client.messages().on<Chat>([&](PeerId, const Chat& c) { clientChat.push_back(c.text); });
    h.server.rpcs().bind("player.hit", [&](PeerId from, f32 dmg, glm::vec3 at) {
        hitFrom = from;
        hitDamage = dmg;
        hitAt = at;
    });
    h.client.rpcs().bind("game.announce", [&](PeerId, std::string text, i32 n) {
        announced = text + std::to_string(n);
    });

    h.client.send(Chat{"hi", 1});
    h.client.callServer("player.hit", 12.5f, glm::vec3(1, 2, 3));
    h.server.multicast("game.announce", std::string("round "), i32{3});
    ASSERT_TRUE(h.runUntil([&] { return !clientChat.empty() && !announced.empty() && hitDamage > 0; }, 2.0));
    EXPECT_EQ(serverChat, std::vector<std::string>{"hi"});
    EXPECT_EQ(clientChat, std::vector<std::string>{"echo:hi"});
    EXPECT_EQ(hitFrom, h.client.clientId());
    EXPECT_EQ(hitDamage, 12.5f);
    EXPECT_EQ(hitAt, glm::vec3(1, 2, 3));
    EXPECT_EQ(announced, "round 3");
}

TEST(NetSession, ClockSyncEstimatesServerTime) {
    MemoryHarness h;
    h.network.setConditions({.latency = 0.05, .jitter = 0.01});
    ASSERT_TRUE(h.server.start());
    ASSERT_TRUE(h.client.connect("m", h.server.port()));
    // MemoryNetwork shares one clock, so both ends use the same time base here; ClockSync.PicksLowestRttSample
    // covers non-zero offsets.
    h.run(5.0);
    ASSERT_TRUE(h.client.clock().synced());
    EXPECT_NEAR(h.client.serverTime(), h.now, 0.01);
    EXPECT_NEAR(h.client.clock().rtt(), 0.1, 0.03);
}

TEST(Replication, SpawnPropertiesAndDeltaSizes) {
    MemoryHarness h;
    ASSERT_TRUE(h.startAndConnect());
    h.client.replication().factory().registerType<TestObject>("TestObject");
    auto obj = std::make_shared<TestObject>();
    for (int i = 0; i < 8; ++i) obj->f[i] = static_cast<f32>(i) * 1.5f;
    obj->pos = {1.f, 2.f, 3.f};
    obj->label = "crate";
    const NetId id = h.server.replication().add(obj);

    stepToNextTick(h);
    const SnapshotStats spawnStats = h.server.lastSnapshotStats(onlyClient(h.server));
    EXPECT_EQ(spawnStats.spawns, 1u);
    ASSERT_TRUE(h.runUntil([&] { return clientObject(h.client, id) != nullptr; }, 1.0));
    TestObject* c = clientObject(h.client, id);
    EXPECT_EQ(c->f[7], 10.5f);
    EXPECT_NEAR(c->pos.z, 3.f, 0.006f);
    EXPECT_EQ(c->label, "crate");

    h.run(0.3); // acks arrive, state becomes the baseline
    stepToNextTick(h);
    const SnapshotStats idle = h.server.lastSnapshotStats(onlyClient(h.server));
    EXPECT_EQ(idle.objectsWritten, 0u);

    obj->f[3] = 99.f;
    stepToNextTick(h);
    const SnapshotStats delta = h.server.lastSnapshotStats(onlyClient(h.server));
    EXPECT_EQ(delta.objectsWritten, 1u);
    EXPECT_EQ(delta.spawns, 0u);
    const u32 fullObjectBytes = spawnStats.bytes - idle.bytes;
    const u32 deltaObjectBytes = delta.bytes - idle.bytes;
    EXPECT_LT(deltaObjectBytes * 3, fullObjectBytes) << "full " << fullObjectBytes << " delta " << deltaObjectBytes;
    // 1 changed float (4 B) + id/flags/baseline/count + 10 mask bits
    EXPECT_LE(deltaObjectBytes, 10u);
    ASSERT_TRUE(h.runUntil([&] { return c->f[3] == 99.f; }, 1.0));
    EXPECT_EQ(c->f[2], 3.f);

    // Sub-resolution changes of a quantised property do not cause sends.
    h.run(0.3);
    obj->pos.x += 0.001f;
    stepToNextTick(h);
    EXPECT_EQ(h.server.lastSnapshotStats(onlyClient(h.server)).objectsWritten, 0u);
}

TEST(Replication, DeltaConvergesUnderLossAndReordering) {
    MemoryHarness h;
    h.network.setConditions({.latency = 0.04, .jitter = 0.03, .loss = 0.2f, .duplicate = 0.05f});
    ASSERT_TRUE(h.startAndConnect());
    h.client.replication().factory().registerType<TestObject>("TestObject");
    std::vector<std::shared_ptr<TestObject>> objs;
    for (int i = 0; i < 5; ++i) {
        objs.push_back(std::make_shared<TestObject>());
        h.server.replication().add(objs.back());
    }
    std::mt19937 rng(5);
    for (int frame = 0; frame < 240; ++frame) {
        auto& o = *objs[rng() % objs.size()];
        // Values that flip back and forth exercise the baseline logic (A -> B -> A).
        o.f[rng() % 8] = static_cast<f32>(rng() % 3);
        o.label = (rng() % 2) ? "a" : "b";
        h.step();
    }
    h.network.setConditions({.latency = 0.04});
    h.run(1.0);
    for (auto& o : objs) {
        TestObject* c = clientObject(h.client, o->netId());
        ASSERT_NE(c, nullptr);
        for (int i = 0; i < 8; ++i) EXPECT_EQ(c->f[i], o->f[i]) << "object " << o->netId() << " f" << i;
        EXPECT_EQ(c->label, o->label);
    }
}

TEST(Replication, DespawnAndDistanceRelevancy) {
    MemoryHarness h;
    ASSERT_TRUE(h.startAndConnect());
    std::vector<NetId> spawned, despawned;
    h.client.replication().factory().registerType<TestObject>("TestObject");
    h.client.replication().onSpawn = [&](NetObject& o) { spawned.push_back(o.netId()); };
    h.client.replication().onDespawn = [&](NetObject& o) { despawned.push_back(o.netId()); };

    auto a = std::make_shared<TestObject>();
    auto b = std::make_shared<TestObject>();
    b->setRelevancy(Relevancy::Distance, 50.f);
    b->pos = {100.f, 0.f, 0.f};
    const NetId ida = h.server.replication().add(a);
    const NetId idb = h.server.replication().add(b);
    h.server.replication().setViewerPosition(onlyClient(h.server), glm::vec3(0.f));
    h.run(0.3);
    EXPECT_EQ(spawned, std::vector<NetId>{ida}); // b is too far away

    b->pos = {10.f, 0.f, 0.f};
    ASSERT_TRUE(h.runUntil([&] { return h.client.replication().find(idb) != nullptr; }, 1.0));
    b->pos = {500.f, 0.f, 0.f};
    ASSERT_TRUE(h.runUntil([&] { return h.client.replication().find(idb) == nullptr; }, 1.0));
    EXPECT_EQ(despawned, std::vector<NetId>{idb});

    h.server.replication().remove(ida);
    ASSERT_TRUE(h.runUntil([&] { return h.client.replication().find(ida) == nullptr; }, 1.0));
    EXPECT_EQ(despawned, (std::vector<NetId>{idb, ida}));
    EXPECT_TRUE(h.client.replication().objects().empty());
}

TEST(Replication, OwnershipIsVisibleOnClient) {
    MemoryHarness h;
    ASSERT_TRUE(h.startAndConnect());
    h.client.replication().factory().registerType<TestObject>("TestObject");
    auto mine = std::make_shared<TestObject>();
    mine->setOwner(onlyClient(h.server));
    mine->setRelevancy(Relevancy::OwnerOnly);
    const NetId id = h.server.replication().add(mine);
    ASSERT_TRUE(h.runUntil([&] { return h.client.replication().find(id) != nullptr; }, 1.0));
    EXPECT_TRUE(h.client.replication().isOwned(*h.client.replication().find(id)));
}

TEST(Replication, BandwidthBudgetPrioritisesHighPriorityObjects) {
    NetServerConfig sc;
    sc.replication.bytesPerTick = 120; // fits ~2 full objects per tick
    MemoryHarness h(sc);
    ASSERT_TRUE(h.startAndConnect());
    h.client.replication().factory().registerType<TestObject>("TestObject");
    std::vector<std::shared_ptr<TestObject>> objs;
    for (int i = 0; i < 12; ++i) {
        objs.push_back(std::make_shared<TestObject>());
        objs.back()->setPriority(i == 0 ? 20.f : 1.f);
        h.server.replication().add(objs.back());
    }
    h.run(2.0); // spawn everything under the budget
    for (auto& o : objs) ASSERT_NE(clientObject(h.client, o->netId()), nullptr);
    for (auto& o : objs) clientObject(h.client, o->netId())->applied = 0;

    u32 maxBytes = 0, deferred = 0;
    for (int t = 0; t < 60; ++t) {
        for (auto& o : objs) {
            for (f32& v : o->f) v += 1.f; // everything dirty every tick
        }
        stepToNextTick(h);
        const SnapshotStats s = h.server.lastSnapshotStats(onlyClient(h.server));
        maxBytes = std::max(maxBytes, s.bytes);
        deferred += s.objectsDeferred;
    }
    h.run(0.2);
    EXPECT_LE(maxBytes, sc.replication.bytesPerTick);
    EXPECT_GT(deferred, 0u);
    const int high = clientObject(h.client, objs[0]->netId())->applied;
    int lowMin = 1 << 30, lowSum = 0;
    for (usize i = 1; i < objs.size(); ++i) {
        const int n = clientObject(h.client, objs[i]->netId())->applied;
        lowMin = std::min(lowMin, n);
        lowSum += n;
    }
    const f32 lowAvg = static_cast<f32>(lowSum) / static_cast<f32>(objs.size() - 1);
    EXPECT_GE(high, 55);           // sent (almost) every tick
    EXPECT_GT(lowMin, 0);          // nobody starves
    EXPECT_GT(high, 4.f * lowAvg); // but low priority objects share the rest
}

TEST(Replication, ClientInterpolatesBetweenSnapshots) {
    MemoryHarness h;
    h.network.setConditions({.latency = 0.02});
    ASSERT_TRUE(h.startAndConnect());
    h.client.replication().factory().registerType<TestObject>("TestObject");
    auto obj = std::make_shared<TestObject>();
    const NetId id = h.server.replication().add(obj);
    // Move at 6 units/s along x, server time = h.now.
    for (int i = 0; i < 240; ++i) {
        obj->pos.x = static_cast<f32>(h.now * 6.0);
        h.step();
    }
    TestObject* c = clientObject(h.client, id);
    ASSERT_NE(c, nullptr);
    ASSERT_GT(c->interp.size(), 5u);
    const f64 renderTime = h.client.renderTime();
    auto s = c->interp.sample(renderTime);
    ASSERT_TRUE(s.has_value());
    EXPECT_FALSE(s->extrapolated);
    // Server sampled position at tick time; expect the curve to match the motion within a frame of movement.
    EXPECT_NEAR(s->position.x, renderTime * 6.0, 6.0 / 60.0);
    // Between two snapshots the result is strictly between their positions.
    const auto& samples = c->interp.samples();
    const f64 mid = (samples[samples.size() - 3].time + samples[samples.size() - 2].time) * 0.5;
    auto m = c->interp.sample(mid);
    EXPECT_GT(m->position.x, samples[samples.size() - 3].position.x);
    EXPECT_LT(m->position.x, samples[samples.size() - 2].position.x);
}

TEST(DedicatedServer, RunsFixedTicksHeadless) {
    MemoryNetwork net;
    DedicatedServerConfig cfg;
    cfg.server.tickRate = 60.f;
    cfg.transportFactory = [&] { return net.createTransport(); };
    cfg.maxTicks = 30;
    cfg.realtime = false;
    u32 ticks = 0;
    f64 totalDt = 0;
    bool started = false;
    const int rc = runDedicatedServer(
        cfg,
        [&](NetServer& s, u32 tick, f64 dt) {
            ++ticks;
            totalDt += dt;
            EXPECT_EQ(tick, s.currentTick() + 1);
        },
        [&](NetServer& s) { started = s.running(); });
    EXPECT_EQ(rc, 0);
    EXPECT_TRUE(started);
    EXPECT_EQ(ticks, 30u);
    EXPECT_NEAR(totalDt, 0.5, 1e-9);

    std::atomic<bool> stop{true};
    cfg.maxTicks = 0;
    cfg.stopFlag = &stop;
    ticks = 0;
    EXPECT_EQ(runDedicatedServer(cfg, [&](NetServer&, u32, f64) { ++ticks; }), 0);
    EXPECT_EQ(ticks, 0u);
}

namespace {
struct InventoryObject : NetObject {
    std::vector<i32> slots;
    std::vector<std::string> tags;
    std::map<std::string, f32> stats;
    std::unordered_map<std::string, std::vector<glm::vec3>> paths;
    InventoryObject() : NetObject("InventoryObject") {
        replicate("slots", slots);
        replicate("tags", tags);
        replicate("stats", stats);
        replicate("paths", paths);
    }
};
} // namespace

TEST(Replication, ArraysAndMapsReplicate) {
    MemoryHarness h;
    ASSERT_TRUE(h.startAndConnect());
    h.client.replication().factory().registerType<InventoryObject>("InventoryObject");
    auto obj = std::make_shared<InventoryObject>();
    obj->slots = {3, -1, 7};
    obj->tags = {"quest", "heavy"};
    obj->stats = {{"hp", 10.f}, {"mana", 2.5f}};
    obj->paths["patrol"] = {glm::vec3(1, 2, 3), glm::vec3(4, 5, 6)};
    const NetId id = h.server.replication().add(obj);
    auto client = [&] { return static_cast<InventoryObject*>(h.client.replication().find(id)); };
    ASSERT_TRUE(h.runUntil([&] { return client() != nullptr; }, 1.0));
    EXPECT_EQ(client()->slots, obj->slots);
    EXPECT_EQ(client()->tags, obj->tags);
    EXPECT_EQ(client()->stats, obj->stats);
    EXPECT_EQ(client()->paths, obj->paths);

    h.run(0.3);
    obj->slots.push_back(42);
    obj->stats.erase("mana");
    obj->paths.clear();
    ASSERT_TRUE(h.runUntil([&] { return client()->slots.size() == 4; }, 1.0));
    EXPECT_EQ(client()->slots.back(), 42);
    EXPECT_EQ(client()->stats.size(), 1u);
    EXPECT_TRUE(client()->paths.empty());

    // Unchanged containers cost nothing.
    h.run(0.3);
    stepToNextTick(h);
    EXPECT_EQ(h.server.lastSnapshotStats(onlyClient(h.server)).objectsWritten, 0u);

    // Corrupt element counts are rejected without allocating.
    BitWriter w;
    w.writeVarU64(u64(1) << 40);
    BitReader r(w.data());
    std::vector<i32> v{1};
    NetCodec<std::vector<i32>>{}.read(r, v);
    EXPECT_TRUE(v.empty());
}
