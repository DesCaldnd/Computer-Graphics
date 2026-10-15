// r.Reflex on the runtime side: marker order and sleep calls per frame (recording renderer), the fallback limiter
// (LatencyLimiter controller + Engine with a simulated GPU-bound renderer). The Vulkan side (VK_NV_low_latency2,
// vkGetLatencyTimingsNV) is tested in engine/rhi/tests/gpu/low_latency_tests.cpp, frame generation + Reflex in
// engine/render/tests/gpu/frame_generation_tests.cpp.
#include "test_util.hpp"

#include <oxwald/core/cvar.hpp>
#include <oxwald/core/time.hpp>
#include <oxwald/runtime/engine.hpp>
#include <oxwald/runtime/latency.hpp>
#include <oxwald/runtime/settings.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using namespace ox;
using SteadyClock = std::chrono::steady_clock;

namespace {

// Restores the persisted latency cvars after a test.
struct LatencyCVarGuard {
    LatencyCVarGuard() : reflex(cvars::reflex().get()), fallback(cvars::reflexFallback().get()) {}
    ~LatencyCVarGuard() {
        cvars::reflex().set(reflex);
        cvars::reflexFallback().set(fallback);
    }
    int reflex, fallback;
};

// Renders by sleeping `renderMs` on the render thread (a GPU-bound frame as the game thread sees it) and records the
// low latency calls of the game thread.
class LatencyRecordingRenderer final : public IRenderer {
public:
    struct Event {
        u64 frame;
        int kind; // 0 sleep, 1 + LatencyMarker, 10 waitForFrameGpu
        u64 renderedAtCall;
    };

    explicit LatencyRecordingRenderer(f64 renderMs, bool driverSleep = false)
        : m_renderMs(renderMs), m_driverSleep(driverSleep) {}

    std::string_view name() const override { return "LatencyRecording"; }
    Status init(Services&, const RenderSurface&) override { return {}; }
    void shutdown() override {}
    void extract(const World&, const FrameContext&) override {}
    void render(const FrameContext& ctx) override {
        const auto start = SteadyClock::now();
        {
            std::lock_guard lock(mutex);
            const auto& s = simStart[ctx.frameIndex % simStart.size()];
            if (s.first == ctx.frameIndex) {
                latencyMs.push_back(std::chrono::duration<f64, std::milli>(start - s.second).count());
            }
            renderStarts.push_back(start);
        }
        // A GPU-bound frame on a virtual GPU timeline: each frame finishes renderMs after the previous one (or after its
        // own start when the GPU was idle). Absolute deadlines on a high-resolution timer: std::this_thread::sleep_for
        // rounds up to the 15.6 ms scheduler tick on Windows, and a late wake-up (loaded machine) does not accumulate.
        const auto renderTime = std::chrono::duration_cast<SteadyClock::duration>(std::chrono::duration<f64, std::milli>(m_renderMs));
        m_gpuDone = std::max(m_gpuDone, start) + renderTime;
        Clock::sleepUntil(m_gpuDone);
        rendered.fetch_add(1);
    }
    void resize(glm::uvec2) override {}
    void settingsChanged() override {}

    f64 latencySleep(u64 frameIndex, i32 mode) override {
        std::lock_guard lock(mutex);
        events.push_back({frameIndex, 0, rendered.load()});
        lastMode = mode;
        return m_driverSleep && mode > 0 ? 0.0 : -1.0;
    }
    void latencyMarker(u64 frameIndex, LatencyMarker marker) override {
        std::lock_guard lock(mutex);
        events.push_back({frameIndex, 1 + int(marker), rendered.load()});
        if (marker == LatencyMarker::SimulationStart) simStart[frameIndex % simStart.size()] = {frameIndex, SteadyClock::now()};
    }
    bool waitForFrameGpu(u64 frameIndex) override {
        std::lock_guard lock(mutex);
        events.push_back({frameIndex, 10, rendered.load()});
        return true;
    }

    std::mutex mutex;
    std::vector<Event> events;
    std::vector<f64> latencyMs; // simulation start -> render start of the same frame
    std::vector<SteadyClock::time_point> renderStarts;
    std::array<std::pair<u64, SteadyClock::time_point>, 64> simStart{};
    std::atomic<u64> rendered{0};
    i32 lastMode = -1;

private:
    f64 m_renderMs;
    bool m_driverSleep;
    SteadyClock::time_point m_gpuDone{};
};

struct EngineRun {
    test::TempDir dir{"latency"};
    Engine engine;
    LatencyRecordingRenderer* renderer = nullptr;

    explicit EngineRun(std::unique_ptr<LatencyRecordingRenderer> r) {
        renderer = r.get();
        engine.setRenderer(std::move(r));
        EngineConfig cfg;
        cfg.headless = true;
        cfg.workerThreads = 2;
        cfg.userDir = dir / "user";
        EXPECT_TRUE(engine.init(cfg));
    }
    ~EngineRun() { engine.shutdown(); }
};

f64 mean(const std::vector<f64>& v, usize skip = 0) {
    if (v.size() <= skip) return 0.0;
    f64 s = 0.0;
    for (usize i = skip; i < v.size(); ++i) s += v[i];
    return s / f64(v.size() - skip);
}

f64 median(std::vector<f64> v, usize skip = 0) {
    if (v.size() <= skip) return 0.0;
    v.erase(v.begin(), v.begin() + std::ptrdiff_t(skip));
    std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(v.size() / 2), v.end());
    return v[v.size() / 2];
}

f64 meanInterval(const std::vector<SteadyClock::time_point>& t, usize skip) {
    if (t.size() < skip + 2) return 0.0;
    return std::chrono::duration<f64, std::milli>(t.back() - t[skip]).count() / f64(t.size() - 1 - skip);
}

} // namespace

TEST(LatencyLimiter, ConvergesToTheQueueingMinusHeadroom) {
    LatencyLimiter limiter;
    const f64 queueWithoutDelay = 8.0; // ms a frame waits behind the GPU when started right away
    f64 delay = 0.0;
    f64 lo = 1e9, hi = 0.0;
    for (int i = 0; i < 60; ++i) {
        delay = limiter.update(std::max(0.0, queueWithoutDelay - delay));
        if (i >= 30) lo = std::min(lo, delay), hi = std::max(hi, delay);
    }
    EXPECT_NEAR(delay, queueWithoutDelay - limiter.config().targetQueueMs, 0.3);
    EXPECT_LT(hi - lo, 0.05) << "settled: no limit cycle around the headroom";
    // CPU bound now (nothing queued): the delay goes back to zero within a few frames.
    for (int i = 0; i < 12; ++i) delay = limiter.update(0.0);
    EXPECT_LT(delay, 0.05);
}

TEST(LatencyLimiter, SingleHitchDoesNotInflateTheDelay) {
    LatencyLimiter limiter;
    f64 delay = 0.0;
    for (int i = 0; i < 40; ++i) delay = limiter.update(std::max(0.0, 4.0 - delay));
    const f64 settled = delay;
    // One frame queued 30 ms longer (e.g. a shader compile on the render thread): the window minimum ignores it.
    delay = limiter.update(30.0 + std::max(0.0, 4.0 - delay));
    EXPECT_LT(delay, settled + 0.6);
}

TEST(LatencyMarkers, OrderedPerFrameAndOneSleepPerFrame) {
    LatencyCVarGuard guard;
    cvars::reflex().set(1);
    EngineRun run(std::make_unique<LatencyRecordingRenderer>(1.0, /*driverSleep*/ true));
    constexpr u64 kFrames = 30;
    for (u64 i = 0; i < kFrames; ++i) run.engine.tick(1.0 / 60.0);
    run.engine.pipeline().flush();
    std::lock_guard lock(run.renderer->mutex);
    // Per frame: sleep, SimulationStart, InputSample, SimulationEnd — in this order and exactly once.
    const std::array<int, 4> expected = {0, 1 + int(LatencyMarker::SimulationStart), 1 + int(LatencyMarker::InputSample),
                                         1 + int(LatencyMarker::SimulationEnd)};
    ASSERT_EQ(run.renderer->events.size(), kFrames * expected.size());
    for (u64 f = 0; f < kFrames; ++f) {
        for (usize k = 0; k < expected.size(); ++k) {
            const auto& e = run.renderer->events[f * expected.size() + k];
            EXPECT_EQ(e.frame, f) << "event " << k;
            EXPECT_EQ(e.kind, expected[k]) << "frame " << f << " event " << k;
        }
    }
    EXPECT_EQ(run.renderer->lastMode, 1);
    const LatencyStats& ls = run.engine.stats().latency;
    EXPECT_TRUE(ls.reflexActive);
    EXPECT_FALSE(ls.limiterActive) << "the driver paced the frame: no engine limiter on top";
}

TEST(LatencyMarkers, OffStillReportsTheModeAndNeverLimits) {
    LatencyCVarGuard guard;
    cvars::reflex().set(0);
    EngineRun run(std::make_unique<LatencyRecordingRenderer>(1.0, true));
    for (int i = 0; i < 10; ++i) run.engine.tick(1.0 / 60.0);
    run.engine.pipeline().flush();
    EXPECT_EQ(run.renderer->lastMode, 0);
    EXPECT_FALSE(run.engine.stats().latency.reflexActive);
    EXPECT_FALSE(run.engine.stats().latency.limiterActive);
    EXPECT_EQ(run.engine.stats().latencySleepMs < 1.0, true);
}

// The fallback limiter (no driver sleep) on a GPU-bound pipeline: input-to-render latency drops, frame time stays
// bound by the render time.
TEST(LatencyLimiterEngine, JustInTimeCutsLatencyAndKeepsFrameTimeBound) {
    LatencyCVarGuard guard;
    constexpr f64 kRenderMs = 8.0;
    constexpr int kFrames = 150;
    constexpr usize kSkip = 40; // controller settling
    auto run = [&](int reflexMode, f64& latency, f64& interval) {
        cvars::reflex().set(reflexMode);
        cvars::reflexFallback().set(1);
        EngineRun r(std::make_unique<LatencyRecordingRenderer>(kRenderMs, false));
        for (int i = 0; i < kFrames; ++i) r.engine.tick(1.0 / 60.0);
        r.engine.pipeline().flush();
        std::lock_guard lock(r.renderer->mutex);
        latency = median(r.renderer->latencyMs, kSkip); // median: robust to scheduler hitches on a loaded machine
        interval = meanInterval(r.renderer->renderStarts, kSkip);
        if (reflexMode > 0) EXPECT_TRUE(r.engine.stats().latency.limiterActive);
    };
    f64 latencyOff = 0, intervalOff = 0, latencyOn = 0, intervalOn = 0;
    run(0, latencyOff, intervalOff);
    run(1, latencyOn, intervalOn);
    std::printf("[ limiter ] render %.1f ms: off latency %.2f ms interval %.2f ms | JIT latency %.2f ms interval %.2f ms\n",
                kRenderMs, latencyOff, intervalOff, latencyOn, intervalOn);
    EXPECT_GT(latencyOff, 1.5 * kRenderMs) << "without limiter a frame waits about one render behind the previous";
    EXPECT_LT(latencyOn, 0.4 * latencyOff);
    EXPECT_LT(latencyOn, 0.5 * kRenderMs) << "just in time: the frame reaches the render thread with about the headroom "
                                             "(LatencyLimiter::Config::targetQueueMs) of queueing";
    // Frame rate: within 25 % of the render time, or of the run without the limiter when the machine is so loaded
    // (ctest -j next to GPU suites) that even that run cannot hold the render time (timer wake-ups late by ms).
    EXPECT_LT(intervalOn, std::max(kRenderMs * 1.25, intervalOff * 1.1 + 0.25))
        << "the limiter must not cost noticeable frame rate";
}

TEST(LatencyLimiterEngine, StrictWaitsForThePreviousFrame) {
    LatencyCVarGuard guard;
    cvars::reflex().set(1);
    cvars::reflexFallback().set(2);
    EngineRun run(std::make_unique<LatencyRecordingRenderer>(3.0, false));
    constexpr u64 kFrames = 20;
    for (u64 i = 0; i < kFrames; ++i) run.engine.tick(1.0 / 60.0);
    run.engine.pipeline().flush();
    std::lock_guard lock(run.renderer->mutex);
    u64 waits = 0;
    for (const auto& e : run.renderer->events) {
        if (e.kind != 10) continue;
        ++waits;
        // waitForFrameGpu(N - 1) is called only once frame N - 1 left the render thread.
        EXPECT_GE(e.renderedAtCall, e.frame + 1);
    }
    EXPECT_EQ(waits, kFrames - 1);
    // Strict = no frame queued: simulation start -> render start is about the game thread's own work.
    EXPECT_LT(mean(run.renderer->latencyMs, 5), 3.0);
}

TEST(LatencySettings, ReflexIsAManagedGraphicsSetting) {
    LatencyCVarGuard guard;
    Settings settings;
    GraphicsSettings g = settings.user().graphics;
    EXPECT_EQ(g.reflex, "On");
    g.reflex = "OnBoost";
    settings.setGraphics(g);
    EXPECT_EQ(cvars::reflex().get(), 2);
    ASSERT_TRUE(CVarRegistry::instance().set("r.Reflex", "Off"));
    settings.captureFromCVars();
    EXPECT_EQ(settings.user().graphics.reflex, "Off");
    EXPECT_EQ(settings.user().cvars.count("r.Reflex"), 0u) << "managed: stored in graphics.reflex, not in cvars";
    ASSERT_TRUE(CVarRegistry::instance().set("r.Reflex", "Auto"));
    EXPECT_EQ(cvars::reflex().get(), 1);
}
