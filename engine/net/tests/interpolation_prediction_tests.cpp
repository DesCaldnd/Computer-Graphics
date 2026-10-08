#include <oxwald/net/interpolation.hpp>
#include <oxwald/net/prediction.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <glm/gtc/quaternion.hpp>

using namespace ox;
using namespace ox::net;

TEST(Interpolation, HermitePositionsAndSlerpRotations) {
    InterpolationBuffer buf;
    const glm::vec3 axis(0, 1, 0);
    for (int i = 0; i < 6; ++i) {
        const f64 t = i * 0.1;
        buf.push({.time = t,
                  .position = glm::vec3(static_cast<f32>(10.0 * t), 0, 0),
                  .rotation = glm::angleAxis(static_cast<f32>(t), axis)});
    }
    // Linear motion is reproduced exactly by Hermite with finite-difference tangents.
    auto s = buf.sample(0.25);
    ASSERT_TRUE(s);
    EXPECT_NEAR(s->position.x, 2.5f, 1e-4f);
    EXPECT_NEAR(glm::angle(s->rotation), 0.25f, 1e-4f);
    EXPECT_FALSE(s->extrapolated);

    // Before the first sample -> clamp.
    EXPECT_NEAR(buf.sample(-1.0)->position.x, 0.f, 1e-6f);
    // Extrapolation is bounded by maxExtrapolation (0.25 s).
    auto e = buf.sample(10.0);
    EXPECT_TRUE(e->extrapolated);
    EXPECT_NEAR(e->position.x, 5.f + 10.f * 0.25f, 1e-3f);

    // Out-of-order samples are ignored.
    buf.push({.time = 0.05, .position = glm::vec3(1000.f)});
    EXPECT_NEAR(buf.sample(0.05)->position.x, 0.5f, 1e-4f);
}

TEST(Interpolation, HermiteFollowsCurvedMotionBetterThanLerp) {
    InterpolationBuffer buf;
    auto circle = [](f64 t) { return glm::vec3(std::cos(t), 0.f, std::sin(t)) * 10.f; };
    for (int i = 0; i < 10; ++i) {
        const f64 t = i * 0.3;
        buf.push({.time = t, .position = circle(t), .velocity = glm::vec3(-std::sin(t), 0.f, std::cos(t)) * 10.f});
    }
    const f64 t = 1.05; // between 0.9 and 1.2
    const glm::vec3 lerp = glm::mix(circle(0.9), circle(1.2), 0.5f);
    const f32 hermiteErr = glm::length(buf.sample(t)->position - circle(t));
    const f32 lerpErr = glm::length(lerp - circle(t));
    EXPECT_LT(hermiteErr * 10.f, lerpErr);
}

TEST(ClockSync, PicksLowestRttSample) {
    ClockSync clock;
    // True offset 50 s, RTT 40 ms; one sample is delayed by queuing on the return path.
    clock.addSample(1.000, 51.020, 1.040);
    clock.addSample(2.000, 52.020, 2.300);
    clock.addSample(3.000, 53.020, 3.040);
    EXPECT_NEAR(clock.offset(), 50.0, 1e-6);
    EXPECT_NEAR(clock.rtt(), 0.04, 1e-9);
    EXPECT_NEAR(clock.serverTime(10.0), 60.0, 1e-6);
}

namespace {
struct MoveState {
    f32 x = 0.f;
    f32 vx = 0.f;
};
struct MoveInput {
    f32 axis = 0.f;
};
void simulate(MoveState& s, const MoveInput& in, f32 dt) {
    s.vx = in.axis * 5.f;
    s.x += s.vx * dt;
}
bool near(const MoveState& a, const MoveState& b) { return std::abs(a.x - b.x) < 1e-4f; }
} // namespace

TEST(Prediction, ReconciliationConvergesAfterMismatch) {
    ClientPrediction<MoveState, MoveInput> client(simulate, near);
    ServerInputQueue<MoveInput> serverQueue;
    MoveState server;
    constexpr f32 dt = 1.f / 60.f;
    constexpr int kLatencyTicks = 6; // one-way
    struct InFlight {
        int deliverAt;
        u32 seq;
        MoveInput input;
    };
    struct Ack {
        int deliverAt;
        u32 seq;
        MoveState state;
    };
    std::deque<InFlight> toServer;
    std::deque<Ack> toClient;
    bool corrected = false;
    f32 maxDivergenceAfterFix = 0.f;
    for (int tick = 0; tick < 240; ++tick) {
        if (tick < 180) {
            const MoveInput in{tick % 40 < 20 ? 1.f : -0.5f};
            const u32 seq = client.applyInput(in, dt);
            toServer.push_back({tick + kLatencyTicks, seq, in});
        }
        while (!toServer.empty() && toServer.front().deliverAt <= tick) {
            serverQueue.receive(toServer.front().seq, toServer.front().input, dt);
            serverQueue.receive(toServer.front().seq, toServer.front().input, dt); // redundancy -> deduped
            toServer.pop_front();
        }
        serverQueue.process([&](const MoveInput& in, f32 d) { simulate(server, in, d); });
        if (tick == 60) {
            server.x += 3.f; // server-only event (e.g. knockback) -> misprediction
        }
        toClient.push_back({tick + kLatencyTicks, serverQueue.lastProcessed(), server});
        while (!toClient.empty() && toClient.front().deliverAt <= tick) {
            corrected |= client.reconcile(toClient.front().seq, toClient.front().state);
            toClient.pop_front();
        }
        if (corrected && client.pending().empty()) {
            maxDivergenceAfterFix = std::max(maxDivergenceAfterFix, std::abs(client.state().x - server.x));
        }
    }
    EXPECT_TRUE(corrected);
    EXPECT_EQ(client.corrections(), 1u); // exactly one mismatch, everything else predicted correctly
    EXPECT_TRUE(client.pending().empty());
    EXPECT_NEAR(client.state().x, server.x, 1e-4f);
    EXPECT_LT(maxDivergenceAfterFix, 1e-4f);
    EXPECT_EQ(client.lastAcked(), client.lastSequence());
}

TEST(Prediction, ReplayKeepsUnackedInputs) {
    ClientPrediction<MoveState, MoveInput> client(simulate, near);
    for (int i = 0; i < 10; ++i) client.applyInput({1.f}, 0.1f);
    EXPECT_NEAR(client.state().x, 5.f, 1e-4f);
    // Server processed 4 inputs but disagrees about the result by +1.
    MoveState auth;
    auth.x = 2.f + 1.f;
    EXPECT_TRUE(client.reconcile(4, auth));
    EXPECT_EQ(client.pending().size(), 6u);
    EXPECT_NEAR(client.state().x, 3.f + 6 * 0.5f, 1e-4f);
    // Stale ack is ignored.
    EXPECT_FALSE(client.reconcile(2, MoveState{}));
    EXPECT_NEAR(client.state().x, 6.f, 1e-4f);
}
