#pragma once

#include <oxwald/core/log.hpp>
#include <oxwald/script/script_events.hpp>
#include <oxwald/script/script_instance.hpp>

#include <gtest/gtest.h>

#include <mutex>
#include <string>
#include <vector>

namespace ox::script::test {

// Captures log output for assertions.
class LogCapture {
public:
    LogCapture() {
        m_id = log::addSink([this](const log::Record& r) {
            std::lock_guard lock(m_mutex);
            m_lines.push_back(std::string(log::levelName(r.level)) + " " + std::string(r.message));
        });
    }
    ~LogCapture() { log::removeSink(m_id); }

    bool contains(std::string_view needle) const {
        std::lock_guard lock(m_mutex);
        for (const auto& l : m_lines) {
            if (l.find(needle) != std::string::npos) return true;
        }
        return false;
    }
    std::string all() const {
        std::lock_guard lock(m_mutex);
        std::string s;
        for (const auto& l : m_lines) s += l + "\n";
        return s;
    }

private:
    int m_id;
    mutable std::mutex m_mutex;
    std::vector<std::string> m_lines;
};

// Runs a snippet in a fresh sandbox and returns its first result.
template <class T>
T eval(ScriptVM& vm, std::string_view code) {
    sol::environment env = vm.createEnvironment();
    ScriptResult r = vm.runString(code, "eval", &env);
    EXPECT_TRUE(r.ok) << r.error;
    if (!r.ok || !r.value.valid()) return T{};
    return r.value.as<T>();
}

} // namespace ox::script::test
