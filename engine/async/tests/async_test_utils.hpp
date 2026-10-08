#pragma once

#include <oxwald/async/async.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

namespace oxtest {

// Counts constructions/destructions to prove that cancelled frames unwind.
struct LiveCounter {
    static inline std::atomic<int> alive{0};
    static inline std::atomic<int> destroyed{0};
    LiveCounter() { alive.fetch_add(1); }
    LiveCounter(const LiveCounter&) { alive.fetch_add(1); }
    ~LiveCounter() {
        alive.fetch_sub(1);
        destroyed.fetch_add(1);
    }
    static void reset() {
        alive = 0;
        destroyed = 0;
    }
};

// Ticks until pred() or the timeout; returns pred().
inline bool tickUntil(ox::CoroutineScheduler& s, const std::function<bool()>& pred, double timeoutSec = 5.0,
                      double dt = 1.0 / 60.0) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeoutSec);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        s.tick(dt);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return true;
}

} // namespace oxtest
