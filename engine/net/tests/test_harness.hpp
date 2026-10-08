#pragma once

#include <oxwald/net/net.hpp>

#include <chrono>
#include <functional>
#include <thread>

namespace ox::net::test {

// Server + client over a deterministic MemoryNetwork with simulated time.
struct MemoryHarness {
    MemoryNetwork network{1234};
    NetServer server;
    NetClient client;
    f64 now = 0.0;
    f64 nextTick = 0.0;

    explicit MemoryHarness(NetServerConfig serverConfig = {}, NetClientConfig clientConfig = {})
        : server(network.createTransport(), serverConfig), client(network.createTransport(), clientConfig) {}

    bool startAndConnect() {
        if (!server.start() || !client.connect("memory", server.port())) {
            return false;
        }
        return runUntil([&] { return client.connected(); }, 5.0);
    }

    // Advances simulated time in 1/120 s steps, ticking the server at its tick rate.
    void step(f64 dt = 1.0 / 120.0) {
        now += dt;
        server.poll(now);
        if (now >= nextTick) {
            server.tick(now);
            nextTick += 1.0 / server.tickRate();
        }
        client.poll(now);
    }

    bool runUntil(const std::function<bool()>& pred, f64 maxSeconds) {
        const f64 end = now + maxSeconds;
        while (now < end) {
            if (pred()) {
                return true;
            }
            step();
        }
        return pred();
    }

    void run(f64 seconds) {
        const f64 end = now + seconds;
        while (now < end) {
            step();
        }
    }
};

// Real time pump for ENet tests.
inline bool pumpReal(const std::function<void(f64)>& poll, const std::function<bool()>& pred, f64 maxSeconds) {
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        const f64 t = std::chrono::duration<f64>(std::chrono::steady_clock::now() - start).count();
        poll(t);
        if (pred()) {
            return true;
        }
        if (t > maxSeconds) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

} // namespace ox::net::test
