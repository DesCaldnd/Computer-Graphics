// Client-side prediction with input replay (PredictedCharacterComponent) over the in-memory transport with
// simulated latency, jitter and loss.
#include "gameplay_test_utils.hpp"

#include <oxwald/scene/prefab.hpp>

using namespace ox;
using namespace ox::gameplay;
using namespace ox::gameplay::test;

namespace {

// Scripted local player: one input per fixed step from a timeline (deterministic).
class ScriptedInput final : public ICharacterInputSource {
public:
    CharacterInput sample(Entity, const PredictedCharacterComponent&, f32 dt) override {
        time += dt;
        ++samples;
        CharacterInput in;
        if (time < 1.0) {
            in.move = {0.f, 1.f}; // forward (-Z)
        } else if (time < 1.6) {
            in.move = {1.f, 0.f}; // right (+X)
            in.jump = !jumped;
            jumped = true;
        } else if (time < 2.2) {
            in.move = {0.7f, -0.7f};
            in.yaw = 0.5f;
        }
        return in;
    }
    f64 time = 0.0;
    u32 samples = 0;
    bool jumped = false;
};

serial::Document playerPrefab() {
    World proto;
    Entity root = proto.create("Player");
    root.add<NetworkIdentityComponent>().netType = "Player";
    auto& nt = root.add<NetworkTransformComponent>();
    nt.predicted = true;
    root.add<CharacterControllerComponent>();
    root.add<PredictedCharacterComponent>().moveSpeed = 4.f;
    return createPrefab(proto, root, {.linkSource = false});
}

struct NetPair {
    explicit NetPair(net::LinkConditions link)
        : network(11), server(testConfig()),
          client(testConfig(), [this](Services& s) { s.addExternal<ICharacterInputSource>(input); }) {
        network.setConditions(link);
        const serial::Document prefab = playerPrefab();
        server.assets.addPrefab("Player", prefab);
        client.assets.addPrefab("Player", prefab);
        server.ground();
        client.ground();
        server.start();
        client.start();
        net::NetServerConfig cfg;
        cfg.tickRate = 30.f;
        EXPECT_TRUE(server.runtime<NetworkRuntime>().startServer(network.createTransport(), cfg));
        EXPECT_TRUE(client.runtime<NetworkRuntime>().connect(network.createTransport(), "memory",
                                                             server.runtime<NetworkRuntime>().server()->port()));
    }
    void step() {
        server.tick(kDt);
        client.tick(kDt);
    }
    bool stepUntil(const std::function<bool()>& pred, int maxFrames) {
        for (int i = 0; i < maxFrames; ++i) {
            if (pred()) return true;
            step();
        }
        return pred();
    }
    // Spawns the player on the server, owned by the client, and waits for the client replica.
    void spawnPlayer() {
        auto& cNet = client.runtime<NetworkRuntime>();
        ASSERT_TRUE(stepUntil([&] { return cNet.client()->connected(); }, 300));
        auto doc = server.assets.prefab("Player");
        ASSERT_TRUE(doc);
        auto inst = instantiatePrefab(server.world, *doc);
        ASSERT_TRUE(inst);
        serverPlayer = *inst;
        serverPlayer.setPosition({0.f, 0.f, 0.f});
        serverPlayer.get<NetworkIdentityComponent>().owner = cNet.client()->clientId();
        ASSERT_TRUE(stepUntil([&] {
            for (auto e : client.world.view<PredictedCharacterComponent>()) clientPlayer = client.world.wrap(e);
            return clientPlayer.valid() && cNet.isPredicted(clientPlayer);
        }, 300));
        EXPECT_FALSE(clientPlayer.has<NetworkProxyTag>()) << "owned + predicted: simulated locally";
    }

    static constexpr f64 kDt = 1.0 / 60.0;
    ScriptedInput input;
    net::MemoryNetwork network;
    GameplayHarness server;
    GameplayHarness client;
    Entity serverPlayer;
    Entity clientPlayer;
};

void expectConverges(NetPair& n, f32 tolerance) {
    // Run the whole input timeline, then let the last acks arrive.
    const f64 startInputTime = n.input.time;
    n.stepUntil([&] { return n.input.time - startInputTime > 3.0; }, 1000);
    n.stepUntil([] { return false; }, 60);
    const glm::vec3 s = n.serverPlayer.worldPosition();
    const glm::vec3 c = n.clientPlayer.worldPosition();
    EXPECT_LT(glm::distance(s, c), tolerance) << "server " << s.x << "," << s.y << "," << s.z << " client " << c.x
                                              << "," << c.y << "," << c.z;
    // The character really moved: forward then right (+ jump), then diagonally.
    EXPECT_LT(s.z, -2.f);
    EXPECT_GT(s.x, 1.f);
    EXPECT_NEAR(s.y, 0.f, 0.05f) << "landed after the jump";
    // Idle frames keep producing inputs; only about one round trip of them may be unacknowledged.
    EXPECT_LE(n.clientPlayer.get<PredictedCharacterComponent>().pendingInputs, 30u) << "inputs get acknowledged";
}

} // namespace

TEST(GameplayPrediction, OwnedCharacterIsPredictedImmediatelyAndConvergesUnderLatency) {
    NetPair n({.latency = 0.06, .jitter = 0.01, .loss = 0.f});
    n.spawnPlayer();
    ASSERT_TRUE(n.clientPlayer.valid());
    EXPECT_TRUE(n.server.runtime<NetworkRuntime>().isPredicted(n.serverPlayer)) << "server runs the client's inputs";

    // Within a few frames of input the client has moved while the server (60 ms away) has not caught up yet.
    const f32 serverZ0 = n.serverPlayer.worldPosition().z;
    for (int i = 0; i < 4; ++i) n.step();
    EXPECT_LT(n.clientPlayer.worldPosition().z, -0.2f) << "client predicted its own input immediately";
    EXPECT_GT(n.serverPlayer.worldPosition().z, n.clientPlayer.worldPosition().z + 0.1f);
    EXPECT_NEAR(n.serverPlayer.worldPosition().z, serverZ0, 0.15f);

    expectConverges(n, 0.02f);
    // Identical simulation on both sides: no (or almost no) replays needed without loss.
    EXPECT_LE(n.clientPlayer.get<PredictedCharacterComponent>().corrections, 3u);
    n.client.runtime<NetworkRuntime>().shutdown();
    n.server.runtime<NetworkRuntime>().shutdown();
}

TEST(GameplayPrediction, ConvergesWithPacketLossAndReplaysAfterServerCorrection) {
    NetPair n({.latency = 0.08, .jitter = 0.02, .loss = 0.2f});
    n.spawnPlayer();
    ASSERT_TRUE(n.clientPlayer.valid());
    expectConverges(n, 0.05f);

    // Authority overrides the prediction: the server teleports the character, the client replays its pending
    // inputs from the corrected state and ends up where the server says.
    const u32 correctionsBefore = n.clientPlayer.get<PredictedCharacterComponent>().corrections;
    n.input.time = 0.0; // run the timeline again (moving while the correction arrives)
    for (int i = 0; i < 20; ++i) n.step();
    n.serverPlayer.setWorldPosition(n.serverPlayer.worldPosition() + glm::vec3(3.f, 0.f, 0.f));
    expectConverges(n, 0.05f);
    EXPECT_GT(n.clientPlayer.get<PredictedCharacterComponent>().corrections, correctionsBefore);
    n.client.runtime<NetworkRuntime>().shutdown();
    n.server.runtime<NetworkRuntime>().shutdown();
}

TEST(GameplayPrediction, OfflineInputDrivesTheCharacterController) {
    ScriptedInput input;
    GameplayHarness h(testConfig(), [&](Services& s) { s.addExternal<ICharacterInputSource>(input); });
    h.ground();
    Entity p = h.world.create("Player");
    p.add<CharacterControllerComponent>();
    p.add<PredictedCharacterComponent>().moveSpeed = 4.f;
    h.start();
    h.run(0.9);
    EXPECT_LT(p.worldPosition().z, -2.5f);
    EXPECT_NEAR(p.get<CharacterControllerComponent>().desiredVelocity.z, -4.f, 1e-3f);
    EXPECT_FALSE(p.has<ExternalCharacterMotionTag>());
}
