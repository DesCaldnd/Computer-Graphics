#include "gameplay_test_utils.hpp"

#include <oxwald/scene/prefab.hpp>

#include <map>
#include <optional>

using namespace ox;
using namespace ox::gameplay;
using namespace ox::gameplay::test;

namespace {

serial::Document cratePrefab() {
    World proto;
    Entity root = proto.create("Crate");
    root.add<NetworkIdentityComponent>().netType = "Crate";
    root.add<NetworkTransformComponent>();
    root.add<AudioSourceComponent>().playOnStart = false; // replicated volume/pitch fields
    auto& rb = root.add<RigidBodyComponent>();
    rb.motionType = physics::MotionType::Kinematic;
    root.add<ColliderComponent>();
    return createPrefab(proto, root, {.linkSource = false});
}

} // namespace

TEST(GameplayNet, EntityReplicatedFromServerWorldToClientWorldWithInterpolation) {
    net::MemoryNetwork network(7);
    GameplayHarness server;
    GameplayHarness client;
    const serial::Document prefab = cratePrefab();
    server.assets.addPrefab("Crate", prefab);
    client.assets.addPrefab("Crate", prefab);

    auto inst = instantiatePrefab(server.world, prefab);
    ASSERT_TRUE(inst);
    Entity crate = *inst;
    crate.setPosition({0.f, 1.f, 0.f});
    crate.get<AudioSourceComponent>().volume = 0.25f;

    server.start();
    client.start();
    auto& sNet = server.runtime<NetworkRuntime>();
    auto& cNet = client.runtime<NetworkRuntime>();
    net::NetServerConfig cfg;
    cfg.tickRate = 30.f;
    ASSERT_TRUE(sNet.startServer(network.createTransport(), cfg));
    ASSERT_TRUE(cNet.connect(network.createTransport(), "memory", sNet.server()->port()));

    const f64 dt = 1.0 / 60.0;
    const f32 speed = 3.f;
    Entity replica;
    auto step = [&] {
        crate.setPosition(crate.localTransform().position + glm::vec3(speed * f32(dt), 0.f, 0.f));
        server.tick(dt);
        client.tick(dt);
        if (!replica.valid()) {
            for (auto e : client.world.view<NetworkIdentityComponent>()) replica = client.world.wrap(e);
        }
    };
    for (int i = 0; i < 120 && !replica.valid(); ++i) step();
    ASSERT_TRUE(replica.valid()) << "client spawned the replicated prefab";
    EXPECT_EQ(replica.get<NetworkIdentityComponent>().netId, crate.get<NetworkIdentityComponent>().netId);
    EXPECT_TRUE(replica.has<NetworkProxyTag>());
    EXPECT_EQ(replica.name(), "Crate");

    for (int i = 0; i < 60; ++i) step(); // let the interpolation buffer fill
    EXPECT_NEAR(replica.get<AudioSourceComponent>().volume, 0.25f, 0.01f) << "attr::Replicated field";

    // Interpolated: ~interpolationDelay (0.1 s) behind the server, moving smoothly every frame although snapshots
    // arrive at 30 Hz.
    f32 prevX = replica.worldPosition().x;
    int increasing = 0;
    for (int i = 0; i < 30; ++i) {
        step();
        const f32 x = replica.worldPosition().x;
        if (x > prevX + 1e-4f) ++increasing;
        prevX = x;
    }
    EXPECT_EQ(increasing, 30);
    EXPECT_NEAR(replica.worldPosition().x, crate.worldPosition().x - speed * 0.1f, 0.15f);
    EXPECT_NEAR(replica.worldPosition().y, 1.f, 0.02f);
    // The client-side proxy body is kinematic and follows the replicated transform.
    auto& cPhys = client.runtime<PhysicsRuntime>();
    EXPECT_EQ(cPhys.physicsWorld().getMotionType(cPhys.bodyOf(replica)), physics::MotionType::Kinematic);

    // Server-side destroy despawns on the client.
    crate.destroy();
    ASSERT_TRUE([&] {
        for (int i = 0; i < 120; ++i) {
            server.tick(dt);
            client.tick(dt);
            if (!client.world.valid(replica.handle())) return true;
        }
        return false;
    }());
    cNet.shutdown();
    sNet.shutdown();
}

namespace {
struct NetInventoryComponent {
    std::vector<std::string> items;
    std::map<std::string, i32> counts;
    std::optional<f32> charge;
};

void registerNetInventory() {
    OX_REFLECT_TYPE(NetInventoryComponent, "Test.NetInventory")
        .field("items", &NetInventoryComponent::items, attr::Replicated{})
        .field("counts", &NetInventoryComponent::counts, attr::Replicated{})
        .field("charge", &NetInventoryComponent::charge, attr::Replicated{});
    ComponentRegistry::instance().add<NetInventoryComponent>();
}
} // namespace

TEST(GameplayNet, ReflectedContainersReplicate) {
    net::MemoryNetwork network(11);
    GameplayHarness server; // registers the gameplay types the prefab needs
    GameplayHarness client;
    registerNetInventory();
    World proto;
    Entity root = proto.create("Bag");
    root.add<NetworkIdentityComponent>().netType = "Bag";
    root.add<NetInventoryComponent>();
    const serial::Document prefab = createPrefab(proto, root, {.linkSource = false});
    server.assets.addPrefab("Bag", prefab);
    client.assets.addPrefab("Bag", prefab);
    auto inst = instantiatePrefab(server.world, prefab);
    ASSERT_TRUE(inst);
    Entity bag = *inst;
    auto& inv = bag.get<NetInventoryComponent>();
    inv.items = {"sword", "rope"};
    inv.counts = {{"arrows", 20}, {"gold", 7}};
    inv.charge = 0.5f;

    server.start();
    client.start();
    auto& sNet = server.runtime<NetworkRuntime>();
    auto& cNet = client.runtime<NetworkRuntime>();
    ASSERT_TRUE(sNet.startServer(network.createTransport(), {}));
    ASSERT_TRUE(cNet.connect(network.createTransport(), "memory", sNet.server()->port()));
    const f64 dt = 1.0 / 60.0;
    Entity replica;
    auto runUntil = [&](auto&& done) {
        for (int i = 0; i < 240; ++i) {
            server.tick(dt);
            client.tick(dt);
            if (!replica.valid()) {
                for (auto e : client.world.view<NetworkIdentityComponent>()) replica = client.world.wrap(e);
            }
            if (replica.valid() && done()) return true;
        }
        return false;
    };
    ASSERT_TRUE(runUntil([&] { return replica.get<NetInventoryComponent>().items.size() == 2; }));
    const auto& got = replica.get<NetInventoryComponent>();
    EXPECT_EQ(got.items, inv.items);
    EXPECT_EQ(got.counts, inv.counts);
    ASSERT_TRUE(got.charge.has_value());
    EXPECT_FLOAT_EQ(*got.charge, 0.5f);

    inv.items.push_back("lamp");
    inv.counts.erase("gold");
    inv.charge.reset();
    ASSERT_TRUE(runUntil([&] { return replica.get<NetInventoryComponent>().items.size() == 3; }));
    EXPECT_EQ(replica.get<NetInventoryComponent>().items.back(), "lamp");
    EXPECT_EQ(replica.get<NetInventoryComponent>().counts.size(), 1u);
    EXPECT_FALSE(replica.get<NetInventoryComponent>().charge.has_value());
    cNet.shutdown();
    sNet.shutdown();
}

TEST(GameplayNet, ServerKeepsHandshakingWhileNoWorldIsAttached) {
    net::MemoryNetwork network(13);
    GameplayHarness server;
    GameplayHarness client;
    server.start();
    client.start();
    auto& sNet = server.runtime<NetworkRuntime>();
    auto& cNet = client.runtime<NetworkRuntime>();
    ASSERT_TRUE(sNet.startServer(network.createTransport(), {}));
    sNet.detach(); // world change in the same frame: the old world is gone, the next one is not attached yet
    ASSERT_TRUE(cNet.connect(network.createTransport(), "memory", sNet.server()->port()));
    const f64 dt = 1.0 / 60.0;
    bool connected = false;
    for (int i = 0; i < 240 && !connected; ++i) {
        sNet.preUpdate(f32(dt));
        sNet.postUpdate(f32(dt));
        client.tick(dt);
        connected = cNet.client() && cNet.client()->connected();
    }
    EXPECT_TRUE(connected) << "the server polls its transport without a world";
    cNet.shutdown();
    sNet.shutdown();
}
