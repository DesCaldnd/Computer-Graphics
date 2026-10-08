#include <oxwald/async/async.hpp>
#include <oxwald/async/net_rpc.hpp>

#include "test_harness.hpp" // engine/net/tests

#include <gtest/gtest.h>

#include <stdexcept>

using namespace ox;
using namespace ox::net;

TEST(AsyncNetRpc, RequestResponseWithTimeout) {
    test::MemoryHarness h;
    serveRequest<int, int, int>(h.server, "math.add", [](PeerId, int a, int b) { return a + b; });
    serveRequest<std::string, std::string>(h.server, "echo.fail", [](PeerId, std::string s) -> std::string {
        throw std::runtime_error("denied: " + s);
    });
    h.server.rpcs().bind("math.slow", [](PeerId, u32, int) { /* never replies */ });
    ASSERT_TRUE(h.startAndConnect());

    RpcCall<int, int, int> add(h.client, "math.add");
    RpcCall<std::string, std::string> fail(h.client, "echo.fail");
    RpcCall<int, int> slow(h.client, "math.slow");

    CoroutineScheduler sched;
    std::optional<int> sum;
    std::string error;
    bool timedOut = false;
    auto task = sched.spawn([&]() -> Task<> {
        sum = co_await realTimeout(add(2, 40), 2.0);
        try {
            co_await fail("door");
        } catch (const AsyncError& e) {
            error = e.what();
        }
        timedOut = !(co_await realTimeout(slow(1), 0.5)).has_value();
    });
    for (int i = 0; i < 600 && task.isRunning(); ++i) {
        h.step();
        sched.tick(1.0 / 120.0);
    }
    EXPECT_EQ(task.status(), CoroutineStatus::Completed);
    EXPECT_EQ(sum, 42);
    EXPECT_EQ(error, "denied: door");
    EXPECT_TRUE(timedOut);
    EXPECT_EQ(add.pending(), 0u);
    EXPECT_EQ(slow.pending(), 1u); // the timed-out request is still tracked until failAll()
    slow.failAll("disconnected");
    EXPECT_EQ(slow.pending(), 0u);
}

TEST(AsyncNetRpc, TimedOutRequestCanBeCancelledIndividually) {
    MemoryHarness h; // public harness (oxwald/net/net_harness.hpp)
    h.server.rpcs().bind("math.slow", [](PeerId, u32, int) { /* never replies */ });
    ASSERT_TRUE(h.startAndConnect());
    RpcCall<int, int> slow(h.client, "math.slow");
    u32 first = 0, second = 0;
    Future<int> a = slow.call(first, 1);
    Future<int> b = slow.call(second, 2);
    ASSERT_NE(first, 0u);
    ASSERT_NE(first, second);
    EXPECT_EQ(slow.pending(), 2u);
    h.run(0.5); // the server never answers: "time out" the first one
    EXPECT_TRUE(slow.cancel(first, "timed out"));
    EXPECT_FALSE(slow.cancel(first)) << "already cancelled";
    EXPECT_EQ(slow.pending(), 1u) << "only the cancelled request is forgotten";
    ASSERT_TRUE(a.isReady());
    EXPECT_TRUE(a.hasError());
    EXPECT_FALSE(b.isReady());
    slow.failAll("shutdown");
    EXPECT_TRUE(b.hasError());
}
