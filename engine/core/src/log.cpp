#include <oxwald/core/assert.hpp>
#include <oxwald/core/log.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ox::log {
namespace {

struct State {
    std::mutex mutex;
    std::vector<std::pair<int, Sink>> sinks;
    int nextId = 1;
#if defined(_WIN32)
    State() { SetConsoleOutputCP(CP_UTF8); } // messages are UTF-8; the console defaults to the OEM code page
#endif
};

State& state() {
    static State s;
    return s;
}

std::atomic<Level> g_minLevel{Level::Info};
std::atomic<bool> g_stderrSink{true};

} // namespace

int addSink(Sink sink) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.sinks.emplace_back(s.nextId, std::move(sink));
    return s.nextId++;
}

void removeSink(int id) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    std::erase_if(s.sinks, [id](const auto& p) { return p.first == id; });
}

void setStderrSinkEnabled(bool enabled) { g_stderrSink.store(enabled, std::memory_order_relaxed); }
bool stderrSinkEnabled() { return g_stderrSink.load(std::memory_order_relaxed); }

void setMinLevel(Level level) { g_minLevel.store(level, std::memory_order_relaxed); }
Level minLevel() { return g_minLevel.load(std::memory_order_relaxed); }

std::string_view levelName(Level level) {
    switch (level) {
    case Level::Trace: return "trace";
    case Level::Debug: return "debug";
    case Level::Info: return "info";
    case Level::Warn: return "warn";
    case Level::Error: return "error";
    case Level::Fatal: return "fatal";
    }
    return "?";
}

void write(Level level, std::string_view category, std::string_view message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    const Record record{level, category, message};
    if (level == Level::Fatal || g_stderrSink.load(std::memory_order_relaxed)) {
        std::fprintf(stderr, "[%.*s] [%.*s] %.*s\n", int(levelName(level).size()), levelName(level).data(),
                     int(category.size()), category.data(), int(message.size()), message.data());
    }
    for (auto& [id, sink] : s.sinks) {
        sink(record);
    }
}

} // namespace ox::log

namespace ox::detail {

void assertFailed(const char* expr, const char* file, int line, const std::string& message) {
    log::write(log::Level::Fatal, "assert",
               std::format("{}:{}: assertion '{}' failed{}{}", file, line, expr, message.empty() ? "" : ": ", message));
    std::abort();
}

} // namespace ox::detail
