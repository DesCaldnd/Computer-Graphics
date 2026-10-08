#pragma once

#include <oxwald/core/types.hpp>

#include <chrono>

namespace ox {

// Monotonic clock. now() is seconds since an arbitrary process-wide epoch (first use).
class Clock {
public:
    using Base = std::chrono::steady_clock;
    using TimePoint = Base::time_point;
    using Duration = Base::duration;

    [[nodiscard]] static TimePoint timePoint() { return Base::now(); }
    [[nodiscard]] static f64 now();
    [[nodiscard]] static f64 seconds(Duration d) { return std::chrono::duration<f64>(d).count(); }
    [[nodiscard]] static f64 secondsBetween(TimePoint a, TimePoint b) { return seconds(b - a); }
    // Blocks the calling thread until `deadline` with sub-millisecond granularity where the OS allows it.
    static void sleepUntil(TimePoint deadline);
};

// Accumulating stopwatch. Starts stopped unless constructed with start = true.
class Stopwatch {
public:
    explicit Stopwatch(bool startNow = false);

    void start();
    void stop();
    void reset();   // stops and zeroes
    void restart(); // zeroes and starts
    [[nodiscard]] bool running() const { return m_running; }

    [[nodiscard]] f64 elapsedSeconds() const;
    [[nodiscard]] f64 elapsedMs() const { return elapsedSeconds() * 1000.0; }
    // Seconds since the previous lap() (or since start for the first lap); keeps running.
    f64 lap();

private:
    Clock::Duration m_accumulated{};
    Clock::TimePoint m_startedAt{};
    Clock::TimePoint m_lastLap{};
    bool m_running = false;
};

// Fixed-timestep accumulator ("Fix your timestep!"):
//   const u32 steps = fixed.advance(frameDt);
//   for (u32 i = 0; i < steps; ++i) simulate(fixed.fixedDt());
//   render(interpolate(prev, curr, fixed.alpha()));
// When more than maxStepsPerFrame steps are pending, the excess whole steps are dropped
// (spiral-of-death guard) and counted in droppedTime().
class FixedTimestep {
public:
    explicit FixedTimestep(f64 fixedDt = 1.0 / 60.0, u32 maxStepsPerFrame = 8);

    // Returns the number of fixed steps to run this frame. Negative/NaN frameDt counts as 0.
    u32 advance(f64 frameDt);

    [[nodiscard]] f64 fixedDt() const { return m_fixedDt; }
    // Interpolation factor between the last two simulated states, in [0,1).
    [[nodiscard]] f64 alpha() const { return m_accumulator / m_fixedDt; }
    [[nodiscard]] f64 accumulator() const { return m_accumulator; }
    [[nodiscard]] u32 maxSteps() const { return m_maxSteps; }
    [[nodiscard]] u64 totalSteps() const { return m_totalSteps; }
    [[nodiscard]] f64 droppedTime() const { return m_droppedTime; }
    // Simulated time = totalSteps * fixedDt (exact when fixedDt never changed).
    [[nodiscard]] f64 simulatedTime() const { return m_simulatedTime; }

    void reset();
    void setFixedDt(f64 fixedDt); // keeps the accumulator (clamped below the new step)
    void setMaxSteps(u32 maxStepsPerFrame);

private:
    f64 m_fixedDt;
    u32 m_maxSteps;
    f64 m_accumulator = 0.0;
    f64 m_droppedTime = 0.0;
    f64 m_simulatedTime = 0.0;
    u64 m_totalSteps = 0;
};

// Per-frame timing with an exponentially smoothed delta for stable FPS display.
class FrameTimer {
public:
    // smoothing: weight of the previous average in [0,1). maxDelta clamps hitches (debugger breaks).
    explicit FrameTimer(f64 smoothing = 0.9, f64 maxDelta = 0.25);

    // Measures the time since the previous tick (0 on the first call) and records it.
    f64 tick();
    // Records an externally measured delta.
    void update(f64 dt);

    [[nodiscard]] f64 deltaSeconds() const { return m_delta; }
    [[nodiscard]] f64 rawDeltaSeconds() const { return m_rawDelta; }
    [[nodiscard]] f64 smoothedDelta() const { return m_smoothed; }
    [[nodiscard]] f64 fps() const { return m_smoothed > 0.0 ? 1.0 / m_smoothed : 0.0; }
    [[nodiscard]] u64 frameCount() const { return m_frameCount; }
    [[nodiscard]] f64 totalTime() const { return m_total; }
    void setSmoothing(f64 smoothing);
    void setMaxDelta(f64 maxDelta) { m_maxDelta = maxDelta; }
    void reset();

private:
    Clock::TimePoint m_last{};
    bool m_hasLast = false;
    f64 m_smoothing;
    f64 m_maxDelta;
    f64 m_delta = 0.0;
    f64 m_rawDelta = 0.0;
    f64 m_smoothed = 0.0;
    f64 m_total = 0.0;
    u64 m_frameCount = 0;
};

} // namespace ox
