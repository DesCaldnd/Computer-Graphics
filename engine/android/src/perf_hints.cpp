#include <oxwald/android/perf_hints.hpp>
#include <oxwald/core/log.hpp>

#include <dlfcn.h>

#include <cmath>
#include <limits>

namespace ox::android {

namespace {

using GetManagerFn = void* (*)();
using CreateSessionFn = void* (*)(void*, const int32_t*, size_t, int64_t);
using UpdateTargetFn = int (*)(void*, int64_t);
using ReportFn = int (*)(void*, int64_t);
using CloseFn = void (*)(void*);
using AcquireThermalFn = void* (*)();
using ReleaseThermalFn = void (*)(void*);
using ThermalStatusFn = int (*)(void*);
using ThermalHeadroomFn = float (*)(void*, int);

template <class F>
F sym(void* lib, const char* name) {
    return lib ? reinterpret_cast<F>(dlsym(lib, name)) : nullptr;
}

} // namespace

PerformanceHints::PerformanceHints() {
    m_lib = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (auto acquire = sym<AcquireThermalFn>(m_lib, "AThermal_acquireManager")) m_thermal = acquire();
}

PerformanceHints::~PerformanceHints() {
    if (m_session) {
        if (auto close = sym<CloseFn>(m_lib, "APerformanceHint_closeSession")) close(m_session);
    }
    if (m_thermal) {
        if (auto release = sym<ReleaseThermalFn>(m_lib, "AThermal_releaseManager")) release(m_thermal);
    }
    if (m_lib) dlclose(m_lib);
}

bool PerformanceHints::startSession(const std::vector<pid_t>& threads, i64 targetFrameNs) {
    auto getManager = sym<GetManagerFn>(m_lib, "APerformanceHint_getManager");
    auto create = sym<CreateSessionFn>(m_lib, "APerformanceHint_createSession");
    if (!getManager || !create || threads.empty()) {
        OX_LOG_INFO("android", "ADPF performance hints unavailable (API < 33)");
        return false;
    }
    m_hintManager = getManager();
    if (!m_hintManager) return false;
    std::vector<int32_t> tids(threads.begin(), threads.end());
    m_session = create(m_hintManager, tids.data(), tids.size(), targetFrameNs);
    m_target = targetFrameNs;
    OX_LOG_INFO("android", "ADPF hint session {} ({} thread(s), target {:.2f} ms)", m_session ? "created" : "unsupported",
                tids.size(), double(targetFrameNs) / 1e6);
    return m_session != nullptr;
}

void PerformanceHints::updateTarget(i64 targetFrameNs) {
    if (!m_session || targetFrameNs == m_target || targetFrameNs <= 0) return;
    if (auto update = sym<UpdateTargetFn>(m_lib, "APerformanceHint_updateTargetWorkDuration")) update(m_session, targetFrameNs);
    m_target = targetFrameNs;
}

void PerformanceHints::reportFrame(i64 actualWorkNs) {
    if (!m_session || actualWorkNs <= 0) return;
    static const auto report = sym<ReportFn>(m_lib, "APerformanceHint_reportActualWorkDuration");
    if (report) report(m_session, actualWorkNs);
}

int PerformanceHints::thermalStatus() const {
    auto status = sym<ThermalStatusFn>(m_lib, "AThermal_getCurrentThermalStatus");
    return m_thermal && status ? status(m_thermal) : -1;
}

float PerformanceHints::thermalHeadroom(int seconds) const {
    auto headroom = sym<ThermalHeadroomFn>(m_lib, "AThermal_getThermalHeadroom");
    return m_thermal && headroom ? headroom(m_thermal, seconds) : std::numeric_limits<float>::quiet_NaN();
}

std::string_view PerformanceHints::thermalStatusName(int s) {
    switch (s) {
    case 0: return "none";
    case 1: return "light";
    case 2: return "moderate";
    case 3: return "severe";
    case 4: return "critical";
    case 5: return "emergency";
    case 6: return "shutdown";
    default: return "unavailable";
    }
}

void PerformanceHints::poll() {
    const int s = thermalStatus();
    if (s == m_lastStatus) return;
    m_lastStatus = s;
    const float h = thermalHeadroom();
    if (s >= 0) {
        OX_LOG_INFO("android", "thermal status: {} (headroom {:.2f})", thermalStatusName(s), std::isnan(h) ? -1.0f : h);
    }
}

} // namespace ox::android
