#include <oxwald/core/profile.hpp>
#include <oxwald/runtime/console.hpp>

#include <algorithm>
#include <cctype>
#include <set>

namespace ox {

namespace {

std::string toLower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return out;
}

bool startsWithNoCase(std::string_view text, std::string_view prefix) {
    if (prefix.size() > text.size()) return false;
    for (usize i = 0; i < prefix.size(); ++i) {
        if (std::tolower((unsigned char)text[i]) != std::tolower((unsigned char)prefix[i])) return false;
    }
    return true;
}

std::vector<std::string> splitStatements(std::string_view line) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;
    for (usize i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '\\' && i + 1 < line.size()) {
            cur += c;
            cur += line[++i];
            continue;
        }
        if (c == '"') quoted = !quoted;
        if (c == ';' && !quoted) {
            out.push_back(std::move(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(std::move(cur));
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.remove_prefix(1);
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.remove_suffix(1);
    return s;
}

} // namespace

Console::Console(usize maxHistory, usize maxOutputLines) : m_maxHistory(maxHistory), m_maxOutput(maxOutputLines) {
    addCommand("help", "Lists commands and cvars (optionally filtered by prefix)", [this](std::span<const std::string> args) {
        const std::string prefix = args.empty() ? std::string() : args[0];
        std::string out;
        for (const auto& [name, cmd] : m_commands) {
            if (startsWithNoCase(name, prefix)) out += name + " - " + cmd.description + "\n";
        }
        for (ICVar* c : CVarRegistry::instance().all()) {
            if (startsWithNoCase(c->name(), prefix)) out += c->name() + " = " + c->toString() + " - " + c->description() + "\n";
        }
        return out;
    });
    addCommand("find", "Finds cvars/commands whose name or description contains the text", [this](std::span<const std::string> args) {
        if (args.empty()) return std::string("usage: find <text>");
        const std::string needle = toLower(args[0]);
        std::string out;
        for (const auto& [name, cmd] : m_commands) {
            if (toLower(name).find(needle) != std::string::npos) out += name + "\n";
        }
        for (ICVar* c : CVarRegistry::instance().all()) {
            if (toLower(c->name()).find(needle) != std::string::npos ||
                toLower(c->description()).find(needle) != std::string::npos) {
                out += c->name() + " = " + c->toString() + "\n";
            }
        }
        return out;
    });
    addCommand("history", "Prints the command history", [this](std::span<const std::string>) {
        std::string out;
        for (const auto& h : m_history) out += h + "\n";
        return out;
    });
    addCommand("clear", "Clears the console output", [this](std::span<const std::string>) {
        clearOutput();
        return std::string();
    });
}

Console::~Console() { stopCaptureLog(); }

void Console::addCommand(std::string name, std::string description, CommandFn fn) {
    m_commands[std::move(name)] = Command{std::move(description), std::move(fn)};
}

void Console::removeCommand(std::string_view name) {
    if (auto it = m_commands.find(name); it != m_commands.end()) m_commands.erase(it);
}

bool Console::hasCommand(std::string_view name) const { return m_commands.find(name) != m_commands.end(); }

Result<std::string> Console::execute(std::string_view line) {
    OX_PROFILE_ZONE();
    const std::string_view trimmed = trim(line);
    if (trimmed.empty()) return std::string();
    if (m_history.empty() || m_history.back() != trimmed) {
        m_history.emplace_back(trimmed);
        while (m_history.size() > m_maxHistory) m_history.pop_front();
    }
    m_historyCursor = m_history.size();
    print("> " + std::string(trimmed), Line::Kind::Input);

    std::string combined;
    std::optional<Error> firstError;
    for (const auto& stmt : splitStatements(trimmed)) {
        if (trim(stmt).empty()) continue;
        auto r = executeOne(trim(stmt));
        if (!r) {
            print(r.error().message, Line::Kind::Error);
            if (!firstError) firstError = r.error();
            continue;
        }
        if (!r->empty()) {
            print(*r);
            if (!combined.empty() && combined.back() != '\n') combined += '\n';
            combined += *r;
        }
    }
    if (firstError) return *firstError;
    return combined;
}

Result<std::string> Console::executeOne(std::string_view line) {
    auto tokens = tokenizeCommandLine(line);
    if (tokens.empty()) return std::string();
    if (auto it = m_commands.find(tokens[0]); it != m_commands.end()) {
        std::vector<std::string> args(tokens.begin() + 1, tokens.end());
        return it->second.fn(args);
    }
    auto& reg = CVarRegistry::instance();
    ICVar* cvar = reg.find(tokens[0]);
    const std::string before = cvar ? cvar->toString() : std::string();
    auto r = reg.execute(line);
    if (r && cvar && tokens.size() > 1 && cvar->toString() != before) cvarChanged.emit(cvar->name());
    if (!r && !cvar && !reg.findCommand(tokens[0])) return makeError("unknown command or cvar '{}'", tokens[0]);
    return r;
}

std::vector<std::string> Console::complete(std::string_view partial) const {
    std::set<std::string> out;
    const std::string_view text = partial;
    const auto space = text.find(' ');
    auto& reg = CVarRegistry::instance();
    if (space == std::string_view::npos) {
        for (const auto& [name, cmd] : m_commands) {
            if (startsWithNoCase(name, text)) out.insert(name);
        }
        for (const auto& name : reg.complete("")) {
            if (startsWithNoCase(name, text)) out.insert(name);
        }
        return {out.begin(), out.end()};
    }
    // Value completion: "r.Upscaler F" -> "r.Upscaler FSR1".
    const std::string name(text.substr(0, space));
    const std::string_view valuePrefix = trim(text.substr(space + 1));
    if (ICVar* c = reg.find(name)) {
        std::vector<std::string> values = c->enumNames();
        if (c->type() == CVarType::Bool) values = {"false", "true"};
        for (const auto& v : values) {
            if (startsWithNoCase(v, valuePrefix)) out.insert(name + " " + v);
        }
    }
    return {out.begin(), out.end()};
}

std::string Console::completeCommonPrefix(std::string_view partial) const {
    const auto candidates = complete(partial);
    if (candidates.empty()) return std::string(partial);
    std::string prefix = candidates.front();
    for (const auto& c : candidates) {
        usize n = 0;
        while (n < prefix.size() && n < c.size() && std::tolower((unsigned char)prefix[n]) == std::tolower((unsigned char)c[n])) ++n;
        prefix.resize(n);
    }
    return prefix.size() >= partial.size() ? prefix : std::string(partial);
}

std::string Console::historyPrev() {
    if (m_history.empty()) return {};
    if (m_historyCursor > 0) --m_historyCursor;
    return m_history[m_historyCursor];
}

std::string Console::historyNext() {
    if (m_historyCursor < m_history.size()) ++m_historyCursor;
    return m_historyCursor < m_history.size() ? m_history[m_historyCursor] : std::string();
}

std::string Console::saveHistory() const {
    std::string out;
    for (const auto& h : m_history) out += h + "\n";
    return out;
}

void Console::loadHistory(std::string_view text) {
    m_history.clear();
    usize start = 0;
    while (start < text.size()) {
        usize end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        auto line = trim(text.substr(start, end - start));
        if (!line.empty()) m_history.emplace_back(line);
        start = end + 1;
    }
    while (m_history.size() > m_maxHistory) m_history.pop_front();
    m_historyCursor = m_history.size();
}

std::vector<Console::Line> Console::output() const {
    std::lock_guard lock(m_outputMutex);
    return {m_output.begin(), m_output.end()};
}

void Console::clearOutput() {
    std::lock_guard lock(m_outputMutex);
    m_output.clear();
}

void Console::print(std::string text, Line::Kind kind) {
    std::lock_guard lock(m_outputMutex);
    m_output.push_back(Line{kind, kind == Line::Kind::Error ? log::Level::Error : log::Level::Info, std::move(text)});
    while (m_output.size() > m_maxOutput) m_output.pop_front();
}

void Console::captureLog(log::Level minLevel) {
    stopCaptureLog();
    m_logSink = log::addSink([this, minLevel](const log::Record& r) {
        if (r.level < minLevel) return;
        std::lock_guard lock(m_outputMutex);
        m_output.push_back(Line{Line::Kind::Log, r.level, "[" + std::string(r.category) + "] " + std::string(r.message)});
        while (m_output.size() > m_maxOutput) m_output.pop_front();
    });
}

void Console::stopCaptureLog() {
    if (m_logSink >= 0) {
        log::removeSink(m_logSink);
        m_logSink = -1;
    }
}

} // namespace ox
