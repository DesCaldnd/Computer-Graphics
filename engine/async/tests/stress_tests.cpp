#include "async_test_utils.hpp"

#include <oxwald/core/time.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>

// Counts every heap allocation of the test process (replaceable global operator new) so the stress test can
// report how many mallocs spawning coroutines costs.
namespace {
std::atomic<unsigned long long> g_heapAllocs{0};
}
void* operator new(std::size_t n) {
    g_heapAllocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) {
        return p;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace ox;
using oxtest::LiveCounter;

namespace {

Task<int> leaf(int i) {
    co_await nextFrame();
    co_return i;
}

Task<void> worker(int i, std::atomic<long long>& sum) {
    LiveCounter c;
    const int v = co_await leaf(i);
    co_await frames(static_cast<u32>(i % 3));
    sum += v;
}

} // namespace

TEST(AsyncStress, HundredThousandTasksNoLeaks) {
    constexpr int kTasks = 100000;
    LiveCounter::reset();
    const CoroutineFrameStats before = coroutineFrameStats();
    std::atomic<long long> sum{0};
    {
        CoroutineScheduler s;
        // Warm-up so the scheduler vectors and pool chunks exist (steady-state measurement).
        for (int i = 0; i < kTasks; ++i) {
            s.spawn(worker(i, sum));
        }
        while (s.liveCount() != 0) {
            s.tick(0.016);
        }
        sum = 0;

        const unsigned long long allocsBefore = g_heapAllocs.load();
        Stopwatch spawnWatch(true);
        for (int i = 0; i < kTasks; ++i) {
            s.spawn(worker(i, sum));
        }
        const double spawnMs = spawnWatch.elapsedMs();
        const unsigned long long spawnAllocs = g_heapAllocs.load() - allocsBefore;
        EXPECT_EQ(LiveCounter::alive, kTasks);
        EXPECT_EQ(s.liveCount(), static_cast<usize>(kTasks));

        Stopwatch runWatch(true);
        int ticks = 0;
        while (s.liveCount() != 0) {
            s.tick(0.016);
            ++ticks;
        }
        const double runMs = runWatch.elapsedMs();
        const unsigned long long totalAllocs = g_heapAllocs.load() - allocsBefore;

        EXPECT_EQ(sum.load(), static_cast<long long>(kTasks) * (kTasks - 1) / 2);
        EXPECT_EQ(LiveCounter::alive, 0);
        EXPECT_LE(ticks, 4);
        const CoroutineFrameStats st = coroutineFrameStats();
        std::printf("[async stress] %d tasks: spawn %.2f ms (%.0f ns/task), run %.2f ms over %d ticks; "
                    "heap allocations: %llu during spawn, %llu total (pool %s, %llu chunks)\n",
                    kTasks, spawnMs, spawnMs * 1e6 / kTasks, runMs, ticks, spawnAllocs, totalAllocs,
                    st.poolEnabled ? "on" : "off", static_cast<unsigned long long>(st.chunkAllocations));
        if (st.poolEnabled) {
            // Frames and records come from the warmed-up pool: only amortised vector growth may allocate.
            EXPECT_LT(spawnAllocs, static_cast<unsigned long long>(kTasks / 100));
        }
    }
    const CoroutineFrameStats after = coroutineFrameStats();
    EXPECT_EQ(after.liveFrames, before.liveFrames); // every frame and record freed
}

TEST(AsyncStress, CancelHundredThousandSuspended) {
    constexpr int kTasks = 100000;
    LiveCounter::reset();
    const u64 liveBefore = coroutineFrameStats().liveFrames;
    {
        CoroutineScheduler s;
        Promise<int> never;
        Future<int> f = never.future();
        for (int i = 0; i < kTasks; ++i) {
            s.spawn(
                [](Future<int> fut) -> Task<> {
                    LiveCounter c;
                    co_await whenAny(toTask(fut), toTask(seconds(1000.0)));
                }(f),
                {.owner = static_cast<u64>(i % 100 + 1)});
        }
        EXPECT_EQ(LiveCounter::alive, kTasks);
        for (u64 o = 1; o <= 100; ++o) {
            s.cancelOwner(o);
        }
        EXPECT_EQ(LiveCounter::alive, 0);
        EXPECT_EQ(s.liveCount(), 0u);
    }
    EXPECT_EQ(coroutineFrameStats().liveFrames, liveBefore);
}
