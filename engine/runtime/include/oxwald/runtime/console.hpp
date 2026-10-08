#pragma once

#include <oxwald/core/cvar.hpp>
#include <oxwald/core/events.hpp>
#include <oxwald/core/log.hpp>

#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <vector>

// In-game console backend (the ImGui UI is a thin view on top of it). Lines are cvar assignments/queries or
// commands — global ox::ConsoleCommand objects or commands registered on this Console instance. Built-ins:
// help [prefix], find <text>, history, clear. Supports `;`-separated lines, history navigation, autocompletion of
// names and of enum/bool cvar values, and captures log output.
namespace ox {

class Console {
public:
    using CommandFn = std::function<std::string(std::span<const std::string> args)>;

    struct Line {
        enum class Kind : u8 { Input, Output, Error, Log };
        Kind kind = Kind::Output;
        log::Level level = log::Level::Info;
        std::string text;
    };

    explicit Console(usize maxHistory = 100, usize maxOutputLines = 2000);
    ~Console();
    Console(const Console&) = delete;
    Console& operator=(const Console&) = delete;

    // Executes a line (added to history). Returns the combined output; errors are reported as Result errors.
    Result<std::string> execute(std::string_view line);

    void addCommand(std::string name, std::string description, CommandFn fn);
    void removeCommand(std::string_view name);
    [[nodiscard]] bool hasCommand(std::string_view name) const;

    // Completion candidates for the partially typed line (full replacement lines, sorted).
    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) const;
    // Longest common prefix of complete() (what Tab inserts).
    [[nodiscard]] std::string completeCommonPrefix(std::string_view partial) const;

    // History (oldest first). Navigation: historyPrev() walks back, historyNext() forward ("" past the newest).
    [[nodiscard]] const std::deque<std::string>& history() const { return m_history; }
    std::string historyPrev();
    std::string historyNext();
    void resetHistoryCursor() { m_historyCursor = m_history.size(); }
    [[nodiscard]] std::string saveHistory() const; // newline-separated
    void loadHistory(std::string_view text);

    // Output buffer (thread-safe: log lines may arrive from any thread).
    [[nodiscard]] std::vector<Line> output() const;
    void clearOutput();
    void print(std::string text, Line::Kind kind = Line::Kind::Output);
    // Mirrors OX_LOG_* records at or above `level` into the output buffer.
    void captureLog(log::Level minLevel = log::Level::Info);
    void stopCaptureLog();

    // Emitted (on the executing thread) after a cvar was changed from the console, with its name.
    Signal<const std::string&> cvarChanged;

private:
    struct Command {
        std::string description;
        CommandFn fn;
    };
    Result<std::string> executeOne(std::string_view line);

    std::map<std::string, Command, std::less<>> m_commands;
    std::deque<std::string> m_history;
    usize m_historyCursor = 0;
    usize m_maxHistory;
    usize m_maxOutput;
    mutable std::mutex m_outputMutex;
    std::deque<Line> m_output;
    int m_logSink = -1;
};

} // namespace ox
