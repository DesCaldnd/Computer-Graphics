#include <oxwald/core/events.hpp>

#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <vector>

using namespace ox;

TEST(Events, SignalConnectEmitDisconnect) {
    Signal<int, const std::string&> sig;
    std::vector<std::string> log;
    Connection a = sig.connect([&](int v, const std::string& s) { log.push_back("a" + std::to_string(v) + s); });
    Connection b = sig.connect([&](int v, const std::string& s) { log.push_back("b" + std::to_string(v) + s); });
    EXPECT_EQ(sig.slotCount(), 2u);
    sig.emit(1, "x");
    EXPECT_EQ(log, (std::vector<std::string>{"a1x", "b1x"}));

    a.disconnect();
    EXPECT_FALSE(a.connected());
    EXPECT_TRUE(b.connected());
    EXPECT_EQ(sig.slotCount(), 1u);
    sig(2, "y");
    EXPECT_EQ(log.back(), "b2y");
    EXPECT_EQ(log.size(), 3u);
    a.disconnect(); // idempotent

    sig.disconnectAll();
    EXPECT_FALSE(b.connected());
    sig.emit(3, "z");
    EXPECT_EQ(log.size(), 3u);
}

TEST(Events, DisconnectDuringEmit) {
    Signal<> sig;
    int first = 0, second = 0, third = 0;
    Connection c2;
    Connection self;
    sig.connect([&] {
        ++first;
        c2.disconnect(); // removes a slot that has not run yet
    });
    c2 = sig.connect([&] { ++second; });
    self = sig.connect([&] {
        ++third;
        self.disconnect(); // removes itself while running
    });
    sig.emit();
    EXPECT_EQ(first, 1);
    EXPECT_EQ(second, 0);
    EXPECT_EQ(third, 1);
    sig.emit();
    EXPECT_EQ(first, 2);
    EXPECT_EQ(third, 1);
    EXPECT_EQ(sig.slotCount(), 1u);
}

TEST(Events, ConnectDuringEmitTakesEffectNextEmit) {
    Signal<> sig;
    int added = 0;
    std::vector<Connection> keep;
    sig.connect([&] { keep.push_back(sig.connect([&] { ++added; })); });
    sig.emit();
    EXPECT_EQ(added, 0);
    sig.emit();
    EXPECT_EQ(added, 1);
}

TEST(Events, ScopedConnectionAndSignalLifetime) {
    Signal<int> sig;
    int sum = 0;
    {
        ScopedConnection sc = sig.connect([&](int v) { sum += v; });
        EXPECT_TRUE(sc.connected());
        sig.emit(5);
        ScopedConnection moved = std::move(sc);
        EXPECT_FALSE(sc.connected()); // NOLINT
        EXPECT_TRUE(moved.connected());
        sig.emit(1);
    }
    sig.emit(100);
    EXPECT_EQ(sum, 6);
    EXPECT_EQ(sig.slotCount(), 0u);

    Connection dangling;
    {
        Signal<> temp;
        dangling = temp.connect([] {});
        EXPECT_TRUE(dangling.connected());
    }
    EXPECT_FALSE(dangling.connected());
    dangling.disconnect(); // safe after the signal died

    Signal<int> src;
    Connection c = src.connect([&](int v) { sum += v; });
    Signal<int> dst = std::move(src);
    dst.emit(4);
    EXPECT_EQ(sum, 10);
    EXPECT_TRUE(c.connected());
    src.emit(1000); // moved-from signal is empty but usable
    EXPECT_EQ(sum, 10);
    ScopedConnection released = dst.connect([](int) {});
    Connection raw = released.release();
    EXPECT_TRUE(raw.connected());
}

TEST(Events, SignalConcurrentEmitAndConnect) {
    Signal<int> sig;
    std::atomic<int> total{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 200; ++i) {
                ScopedConnection c = sig.connect([&](int v) { total.fetch_add(v); });
                sig.emit(1);
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    EXPECT_GE(total.load(), 800); // each emit reaches at least its own slot
    EXPECT_EQ(sig.slotCount(), 0u);
}

namespace {
struct Damage {
    int amount;
};
struct Spawned {
    std::string name;
};
} // namespace

TEST(Events, EventBusPublishAndQueue) {
    EventBus bus;
    std::vector<std::string> log;
    Connection d = bus.subscribe<Damage>([&](const Damage& e) { log.push_back("D" + std::to_string(e.amount)); });
    ScopedConnection s = bus.subscribe<Spawned>([&](const Spawned& e) { log.push_back("S" + e.name); });
    EXPECT_EQ(bus.subscriberCount<Damage>(), 1u);

    bus.publish(Damage{5});
    EXPECT_EQ(log, (std::vector<std::string>{"D5"}));

    bus.enqueue(Spawned{"orc"});
    bus.enqueue(Damage{1});
    bus.enqueue(Spawned{"elf"});
    EXPECT_EQ(log.size(), 1u);
    EXPECT_EQ(bus.pendingCount(), 3u);
    EXPECT_EQ(bus.dispatch(), 3u);
    EXPECT_EQ(log, (std::vector<std::string>{"D5", "Sorc", "D1", "Self"}));

    d.disconnect();
    bus.publish(Damage{9});
    EXPECT_EQ(log.size(), 4u);

    // Events enqueued during dispatch wait for the next dispatch.
    Connection chain = bus.subscribe<Damage>([&](const Damage& e) {
        if (e.amount > 0) {
            bus.enqueue(Damage{e.amount - 1});
        }
    });
    bus.enqueue(Damage{2});
    EXPECT_EQ(bus.dispatch(), 1u);
    EXPECT_EQ(bus.dispatch(), 1u);
    EXPECT_EQ(bus.dispatch(), 1u);
    EXPECT_EQ(bus.dispatch(), 0u);

    bus.enqueue(Damage{0});
    bus.clearQueue();
    EXPECT_EQ(bus.dispatch(), 0u);
}

TEST(Events, EventBusEnqueueFromThreads) {
    EventBus bus;
    int received = 0;
    long long sum = 0;
    Connection c = bus.subscribe<Damage>([&](const Damage& e) {
        ++received;
        sum += e.amount;
    });
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&bus, t] {
            for (int i = 0; i < 250; ++i) {
                bus.enqueue(Damage{t * 1000 + i});
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    EXPECT_EQ(bus.dispatch(), 1000u);
    EXPECT_EQ(received, 1000);
    long long expected = 0;
    for (int t = 0; t < 4; ++t) {
        for (int i = 0; i < 250; ++i) {
            expected += t * 1000 + i;
        }
    }
    EXPECT_EQ(sum, expected);
}
