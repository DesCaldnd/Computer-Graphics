#include <oxwald/runtime/latency.hpp>

#include <algorithm>

namespace ox {

f64 LatencyLimiter::update(f64 queueMs) {
    const u32 window = std::clamp(m_config.window, 1u, kMaxWindow);
    m_history[m_pos] = std::max(queueMs, 0.0);
    m_pos = (m_pos + 1) % window;
    m_count = std::min(m_count + 1, window);
    f64 minQueue = m_history[0];
    for (u32 i = 1; i < m_count; ++i) minQueue = std::min(minQueue, m_history[i]);
    const f64 lastQueue = m_history[(m_pos + window - 1) % window];
    if (lastQueue <= 0.05) {
        // Nothing queued at all: CPU bound, every ms of delay is now a lost frame-rate ms. Back off fast.
        m_delayMs -= std::max(m_config.decayMs, 0.5 * m_delayMs);
    } else {
        // Proportional in both directions around the headroom. (A one-sided controller that only ever adds delay and
        // decays by a fixed step whenever the excess reaches zero never settles: at the equilibrium the excess rounds
        // to <= 0, the decay drops the delay, the window minimum keeps that sample for `window` frames and the delay
        // keeps falling, a limit cycle of about 1 ms below the target.)
        m_delayMs += m_config.gain * (minQueue - m_config.targetQueueMs);
    }
    m_delayMs = std::clamp(m_delayMs, 0.0, m_config.maxDelayMs);
    return m_delayMs;
}

void LatencyLimiter::reset() {
    m_history.fill(0.0);
    m_count = 0;
    m_pos = 0;
    m_delayMs = 0.0;
}

} // namespace ox
