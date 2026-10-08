#pragma once

#include <format>
#include <functional>
#include <string>
#include <string_view>

namespace ox::log {

enum class Level { Trace, Debug, Info, Warn, Error, Fatal };

struct Record {
    Level level;
    std::string_view category;
    std::string_view message;
};

using Sink = std::function<void(const Record&)>;

// Sinks are called under a mutex; the default sink prints to stderr.
int addSink(Sink sink);
void removeSink(int id);
void setMinLevel(Level level);
Level minLevel();
std::string_view levelName(Level level);

void write(Level level, std::string_view category, std::string_view message);

template <class... Args>
void print(Level level, std::string_view category, std::format_string<Args...> fmt, Args&&... args) {
    if (level < minLevel()) {
        return;
    }
    write(level, category, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace ox::log

#define OX_LOG_TRACE(cat, ...) ::ox::log::print(::ox::log::Level::Trace, cat, __VA_ARGS__)
#define OX_LOG_DEBUG(cat, ...) ::ox::log::print(::ox::log::Level::Debug, cat, __VA_ARGS__)
#define OX_LOG_INFO(cat, ...)  ::ox::log::print(::ox::log::Level::Info, cat, __VA_ARGS__)
#define OX_LOG_WARN(cat, ...)  ::ox::log::print(::ox::log::Level::Warn, cat, __VA_ARGS__)
#define OX_LOG_ERROR(cat, ...) ::ox::log::print(::ox::log::Level::Error, cat, __VA_ARGS__)
