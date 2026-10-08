#include "gameplay_test_utils.hpp"

#include <oxwald/scene/prefab.hpp>

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
