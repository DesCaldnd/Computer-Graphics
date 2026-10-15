#pragma once

// Android Dynamic Performance Framework (ADPF), resolved at run time with dlsym so the APK still runs on API 28+:
//   * APerformanceHint (API 33): a hint session for the game thread with the frame budget as target; each frame
//     reports its actual CPU work duration so the scheduler raises/lowers clocks before the game misses frames.
//   * AThermal (API 30/31): thermal status (none .. shutdown) and headroom forecast; logged when they change and
//     exposed for quality scaling (e.g. drop r.ScreenPercentage when headroom > 0.85).
#include <oxwald/core/types.hpp>

#include <sys/types.h>

#include <string_view>
#include <vector>

namespace ox::android {

class PerformanceHints {
public:
    PerformanceHints();
    ~PerformanceHints();
    PerformanceHints(const PerformanceHints&) = delete;
    PerformanceHints& operator=(const PerformanceHints&) = delete;

    // Creates the hint session (false when ADPF is unavailable: API < 33 or the device has no support).
    bool startSession(const std::vector<pid_t>& threads, i64 targetFrameNs);
    void updateTarget(i64 targetFrameNs);
    void reportFrame(i64 actualWorkNs);
    [[nodiscard]] bool sessionActive() const { return m_session != nullptr; }

    // 0 none, 1 light, 2 moderate, 3 severe, 4 critical, 5 emergency, 6 shutdown; -1 unavailable.
    [[nodiscard]] int thermalStatus() const;
    // Forecast in `seconds` (1.0 = throttling threshold); NaN when unavailable.
    [[nodiscard]] float thermalHeadroom(int seconds = 10) const;
    [[nodiscard]] static std::string_view thermalStatusName(int status);
    // Logs thermal changes (call about once a second).
    void poll();

private:
    void* m_lib = nullptr;
    void* m_hintManager = nullptr;
    void* m_session = nullptr;
    void* m_thermal = nullptr;
    int m_lastStatus = -2;
    i64 m_target = 0;
};

} // namespace ox::android
