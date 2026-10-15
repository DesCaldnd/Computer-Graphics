#pragma once

// Deadline-based frame pacing for a millisecond-resolution timer: each tick is scheduled against an accumulating
// deadline (deadline += period), so the average tick rate equals the target exactly even when the period is not a
// whole number of milliseconds (a plain 1000 / fps interval gives 125 fps for a 120 fps limit, 166 for 144). A tick
// up to kCatchUpPeriods behind (a UI-thread hiccup of one frame) keeps the schedule, so the next tick comes at once
// and the average rate holds; restarting on every late tick lost ~7 % of the ticks at 100 Hz (92-94 fps measured).
// A tick further behind (a long frame, a blocked event loop) restarts the schedule from now instead of bursting.

#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace ox::editor {

class FramePacer {
public:
    static constexpr qint64 kCatchUpPeriods = 2;

    // Delay in milliseconds until the next tick; nowNs = monotonic time of this tick, fps <= 0 = unlimited (0 ms).
    int next(qint64 nowNs, double fps) {
        if (fps <= 0.0) {
            m_deadline = 0;
            return 0;
        }
        const auto period = qint64(std::llround(1e9 / fps));
        if (m_deadline == 0 || nowNs - m_deadline > kCatchUpPeriods * period) m_deadline = nowNs + period;
        else m_deadline += period;
        return int(std::max<qint64>(0, (m_deadline - nowNs) / 1'000'000));
    }
    void reset() { m_deadline = 0; }

private:
    qint64 m_deadline = 0;
};

} // namespace ox::editor
