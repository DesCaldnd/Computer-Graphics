#include <oxwald/android/android_log.hpp>
#include <oxwald/core/log.hpp>

#include <android/log.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <thread>

namespace ox::android {

namespace {

int priorityOf(log::Level level) {
    switch (level) {
    case log::Level::Trace: return ANDROID_LOG_VERBOSE;
    case log::Level::Debug: return ANDROID_LOG_DEBUG;
    case log::Level::Info: return ANDROID_LOG_INFO;
    case log::Level::Warn: return ANDROID_LOG_WARN;
    case log::Level::Error: return ANDROID_LOG_ERROR;
    case log::Level::Fatal: return ANDROID_LOG_FATAL;
    }
    return ANDROID_LOG_INFO;
}

int g_sink = -1;

} // namespace

void installLogcatSink(const char* tag) {
    if (g_sink >= 0) return;
    log::setStderrSinkEnabled(false);
    g_sink = log::addSink([tag](const log::Record& r) {
        // logcat truncates around 4 KB per entry: split long messages (shader errors, cvar dumps).
        std::string line = "[" + std::string(r.category) + "] ";
        std::string_view msg = r.message;
        constexpr std::size_t kChunk = 3800;
        do {
            const std::string_view part = msg.substr(0, kChunk);
            __android_log_write(priorityOf(r.level), tag, (line + std::string(part)).c_str());
            msg.remove_prefix(part.size());
            line = "  ";
        } while (!msg.empty());
    });
}

bool redirectStdioToLogcat(const char* tag) {
    static bool done = false;
    if (done) return true;
    int fds[2];
    if (pipe(fds) != 0) return false;
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    std::thread([fd = fds[0], tag] {
        char buffer[1024];
        std::string pending;
        for (;;) {
            const ssize_t n = read(fd, buffer, sizeof(buffer));
            if (n <= 0) break;
            pending.append(buffer, std::size_t(n));
            std::size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                const std::string line = pending.substr(0, nl);
                const bool warn = line.find("error") != std::string::npos || line.find("ERROR") != std::string::npos;
                __android_log_write(warn ? ANDROID_LOG_WARN : ANDROID_LOG_INFO, tag, line.c_str());
                pending.erase(0, nl + 1);
            }
        }
    }).detach();
    done = true;
    return true;
}

} // namespace ox::android
