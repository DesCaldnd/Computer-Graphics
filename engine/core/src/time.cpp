#include <oxwald/core/time.hpp>

#include <oxwald/core/assert.hpp>

#include <algorithm>
#include <cmath>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

namespace ox {

f64 Clock::now() {
    static const TimePoint kEpoch = Base::now();
    return seconds(Base::now() - kEpoch);
}

void Clock::sleepUntil(TimePoint deadline) {
#if defined(_WIN32)
    // Sleep() and std::this_thread::sleep_until round up to the scheduler tick (15.6 ms by default); a
    // high-resolution waitable timer (Windows 10 1803+) does not.
    thread_local const HANDLE timer =
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const Duration remaining = deadline - Base::now();
    if (remaining <= Duration::zero()) return;
    if (timer) {
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<i64>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count() / 100);
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
#endif
    std::this_thread::sleep_until(deadline);
}

// ---------------------------------------------------------------------------------------------

Stopwatch::Stopwatch(bool startNow) {
    if (startNow) {
        start();
    }
}

void Stopwatch::start() {
    if (m_running) {
        return;
    }
    m_running = true;
    m_startedAt = Clock::timePoint();
    if (m_lastLap == Clock::TimePoint{}) {
        m_lastLap = m_startedAt;
    }
}

void Stopwatch::stop() {
    if (!m_running) {
        return;
    }
    m_accumulated += Clock::timePoint() - m_startedAt;
    m_running = false;
}

void Stopwatch::reset() {
    m_accumulated = {};
    m_running = false;
    m_lastLap = {};
}

void Stopwatch::restart() {
    reset();
    start();
}

f64 Stopwatch::elapsedSeconds() const {
    Clock::Duration d = m_accumulated;
    if (m_running) {
        d += Clock::timePoint() - m_startedAt;
    }
    return Clock::seconds(d);
}

f64 Stopwatch::lap() {
    const Clock::TimePoint now = Clock::timePoint();
    const Clock::TimePoint from = m_lastLap == Clock::TimePoint{} ? now : m_lastLap;
    m_lastLap = now;
    return Clock::secondsBetween(from, now);
}

// ---------------------------------------------------------------------------------------------

FixedTimestep::FixedTimestep(f64 fixedDt, u32 maxStepsPerFrame) : m_fixedDt(fixedDt), m_maxSteps(maxStepsPerFrame) {
    OX_ASSERT(fixedDt > 0.0, "FixedTimestep: fixedDt must be positive (got {})", fixedDt);
    OX_ASSERT(maxStepsPerFrame > 0, "FixedTimestep: maxStepsPerFrame must be > 0");
}

u32 FixedTimestep::advance(f64 frameDt) {
    if (!(frameDt > 0.0)) { // also rejects NaN
        frameDt = 0.0;
    }
    m_accumulator += frameDt;
    // Tiny tolerance so that e.g. 3 * (1/60) accumulated in floating point yields 3 steps.
    const f64 tolerance = m_fixedDt * 1e-9;
    f64 stepsF = std::floor((m_accumulator + tolerance) / m_fixedDt);
    u32 steps = 0;
    if (stepsF > static_cast<f64>(m_maxSteps)) {
        steps = m_maxSteps;
        const f64 excess = m_accumulator - static_cast<f64>(m_maxSteps) * m_fixedDt;
        // Drop the whole excess steps but keep the fractional remainder for smooth interpolation.
        const f64 keep = std::fmod(excess, m_fixedDt);
        m_droppedTime += excess - keep;
        m_accumulator = keep;
    } else {
        steps = static_cast<u32>(stepsF);
        m_accumulator -= static_cast<f64>(steps) * m_fixedDt;
    }
    m_accumulator = std::clamp(m_accumulator, 0.0, std::nextafter(m_fixedDt, 0.0));
    m_totalSteps += steps;
    m_simulatedTime += static_cast<f64>(steps) * m_fixedDt;
    return steps;
}

void FixedTimestep::reset() {
    m_accumulator = 0.0;
    m_droppedTime = 0.0;
    m_simulatedTime = 0.0;
    m_totalSteps = 0;
}

void FixedTimestep::setFixedDt(f64 fixedDt) {
    OX_ASSERT(fixedDt > 0.0, "FixedTimestep: fixedDt must be positive (got {})", fixedDt);
    m_fixedDt = fixedDt;
    m_accumulator = std::min(m_accumulator, std::nextafter(m_fixedDt, 0.0));
}

void FixedTimestep::setMaxSteps(u32 maxStepsPerFrame) {
    OX_ASSERT(maxStepsPerFrame > 0, "FixedTimestep: maxStepsPerFrame must be > 0");
    m_maxSteps = maxStepsPerFrame;
}

// ---------------------------------------------------------------------------------------------

FrameTimer::FrameTimer(f64 smoothing, f64 maxDelta) : m_maxDelta(maxDelta) { setSmoothing(smoothing); }

f64 FrameTimer::tick() {
    const Clock::TimePoint now = Clock::timePoint();
    const f64 dt = m_hasLast ? Clock::secondsBetween(m_last, now) : 0.0;
    m_last = now;
    m_hasLast = true;
    update(dt);
    return m_delta;
}

void FrameTimer::update(f64 dt) {
    if (!(dt > 0.0)) {
        dt = 0.0;
    }
    m_rawDelta = dt;
    m_delta = m_maxDelta > 0.0 ? std::min(dt, m_maxDelta) : dt;
    m_total += m_delta;
    ++m_frameCount;
    if (m_delta > 0.0) {
        m_smoothed = m_smoothed <= 0.0 ? m_delta : m_smoothed * m_smoothing + m_delta * (1.0 - m_smoothing);
    }
}

void FrameTimer::setSmoothing(f64 smoothing) { m_smoothing = std::clamp(smoothing, 0.0, 0.999); }

void FrameTimer::reset() {
    m_hasLast = false;
    m_delta = m_rawDelta = m_smoothed = m_total = 0.0;
    m_frameCount = 0;
}

} // namespace ox
