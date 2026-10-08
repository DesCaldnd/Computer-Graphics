#include <oxwald/core/cvar.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/core/scalability.hpp>

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <system_error>

namespace ox {

// ---- ICVar -------------------------------------------------------------------------------------------------

ICVar::ICVar(std::string_view name, std::string_view description, CVarFlags flags, Scalability group,
             std::vector<std::string> enumNames, std::optional<double> min, std::optional<double> max)
    : m_name(name), m_description(description), m_flags(flags), m_group(group), m_enumNames(std::move(enumNames)),
      m_min(min), m_max(max) {}

ICVar::~ICVar() {
    if (m_registered) CVarRegistry::instance().remove(*this);
}

void ICVar::registerSelf() {
    CVarRegistry::instance().add(*this);
    m_registered = true;
}

bool ICVar::allowed(CVarSource source) const {
    if (source == CVarSource::Code || source == CVarSource::Default) return true;
    if (hasFlag(CVarFlags::ReadOnly)) return false;
    if (source == CVarSource::Console && hasFlag(CVarFlags::Cheat) && !CVarRegistry::instance().cheatsEnabled()) {
        return false;
    }
    return true;
}

void ICVar::noteChanged(CVarSource source) {
    m_lastSource.store(source, std::memory_order_relaxed);
    if (!m_registered) return;
    if (hasFlag(CVarFlags::RequiresRestart)) CVarRegistry::instance().markRestartRequired();
    CVarRegistry::instance().notifyChanged(*this, source);
}

// ---- CVar<T> parsing ---------------------------------------------------------------------------------------

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        const char x = (a[i] >= 'A' && a[i] <= 'Z') ? char(a[i] - 'A' + 'a') : a[i];
        const char y = (b[i] >= 'A' && b[i] <= 'Z') ? char(b[i] - 'A' + 'a') : b[i];
        if (x != y) return false;
    }
    return true;
}

} // namespace

namespace detail {

std::optional<bool> parseCVarBool(std::string_view text) {
    text = trim(text);
    if (text == "1" || iequals(text, "true") || iequals(text, "on") || iequals(text, "yes")) return true;
    if (text == "0" || iequals(text, "false") || iequals(text, "off") || iequals(text, "no")) return false;
    return std::nullopt;
}

std::optional<int> parseCVarInt(std::string_view text, const std::vector<std::string>& enumNames,
                                const std::vector<std::pair<std::string, int>>& aliases) {
    text = trim(text);
    for (usize i = 0; i < enumNames.size(); ++i) {
        if (iequals(enumNames[i], text)) return int(i);
    }
    for (const auto& [alias, value] : aliases) {
        if (iequals(alias, text)) return value;
    }
    int v = 0;
    auto [p, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
    if (ec == std::errc{} && p == text.data() + text.size() && !text.empty()) return v;
    // Accept "2.0" style input for ints.
    std::string s(text);
    char* end = nullptr;
    const double d = std::strtod(s.c_str(), &end);
    if (!s.empty() && end && *end == '\0') return int(d);
    return std::nullopt;
}

std::optional<float> parseCVarFloat(std::string_view text) {
    std::string s(trim(text));
    char* end = nullptr;
    const float v = std::strtof(s.c_str(), &end);
    if (s.empty() || !end || *end != '\0') return std::nullopt;
    return v;
}

std::string formatCVarFloat(float v) {
    char buf[32];
    for (int prec = 6; prec <= 9; ++prec) {
        std::snprintf(buf, sizeof(buf), "%.*g", prec, double(v));
        if (std::strtof(buf, nullptr) == v) break;
    }
    return buf;
}

} // namespace detail

template class CVar<bool>;
template class CVar<int>;
template class CVar<float>;
template class CVar<std::string>;

// ---- ConsoleCommand ----------------------------------------------------------------------------------------

ConsoleCommand::ConsoleCommand(std::string_view name, std::string_view description, Fn fn)
    : m_name(name), m_description(description), m_fn(std::move(fn)) {
    CVarRegistry::instance().addCommand(*this);
}

ConsoleCommand::~ConsoleCommand() { CVarRegistry::instance().removeCommand(*this); }

// ---- CVarRegistry ------------------------------------------------------------------------------------------

CVarRegistry& CVarRegistry::instance() {
    // Leaked on purpose: static CVar objects in other translation units unregister during exit.
    static CVarRegistry* registry = new CVarRegistry();
    return *registry;
}

void CVarRegistry::add(ICVar& cvar) {
    std::optional<nlohmann::json> pending;
    {
        std::lock_guard lock(m_mutex);
        for (auto* c : m_cvars) {
            // Lookups are case-insensitive, so names differing only in case would shadow each other.
            OX_ASSERT(!iequals(c->name(), cvar.name()), "cvar '{}' registered twice (as '{}')", cvar.name(), c->name());
        }
        for (auto* cmd : m_commands) {
            OX_ASSERT(!iequals(cmd->name(), cvar.name()), "cvar '{}' clashes with console command '{}'", cvar.name(),
                      cmd->name());
        }
        m_cvars.push_back(&cvar);
        for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
            if (it->first == cvar.name()) {
                pending = std::move(it->second);
                m_pending.erase(it);
                break;
            }
        }
    }
    if (cvar.group() != Scalability::None) scalability::detail::onCVarRegistered(cvar);
    if (pending && !cvar.setFromJson(*pending, CVarSource::Config)) {
        OX_LOG_WARN("cvar", "ignoring saved value {} for '{}'", pending->dump(), cvar.name());
    }
}

void CVarRegistry::remove(ICVar& cvar) {
    std::lock_guard lock(m_mutex);
    std::erase(m_cvars, &cvar);
}

void CVarRegistry::addCommand(ConsoleCommand& cmd) {
    std::lock_guard lock(m_mutex);
    for (auto* c : m_commands) {
        OX_ASSERT(!iequals(c->name(), cmd.name()), "console command '{}' registered twice (as '{}')", cmd.name(),
                  c->name());
    }
    for (auto* c : m_cvars) {
        OX_ASSERT(!iequals(c->name(), cmd.name()), "console command '{}' clashes with cvar '{}'", cmd.name(), c->name());
    }
    m_commands.push_back(&cmd);
}

void CVarRegistry::removeCommand(ConsoleCommand& cmd) {
    std::lock_guard lock(m_mutex);
    std::erase(m_commands, &cmd);
}

ICVar* CVarRegistry::find(std::string_view name) const {
    std::lock_guard lock(m_mutex);
    for (auto* c : m_cvars) {
        if (iequals(c->name(), name)) return c;
    }
    return nullptr;
}

const ConsoleCommand* CVarRegistry::findCommand(std::string_view name) const {
    std::lock_guard lock(m_mutex);
    for (auto* c : m_commands) {
        if (iequals(c->name(), name)) return c;
    }
    return nullptr;
}

std::vector<ICVar*> CVarRegistry::all() const {
    std::lock_guard lock(m_mutex);
    std::vector<ICVar*> out = m_cvars;
    std::sort(out.begin(), out.end(), [](auto* a, auto* b) { return a->name() < b->name(); });
    return out;
}

std::vector<ICVar*> CVarRegistry::inGroup(Scalability group) const {
    std::vector<ICVar*> out;
    for (auto* c : all()) {
        if (c->group() == group) out.push_back(c);
    }
    return out;
}

std::vector<std::string> CVarRegistry::complete(std::string_view prefix) const {
    std::lock_guard lock(m_mutex);
    std::vector<std::string> out;
    auto matches = [&](const std::string& n) { return n.size() >= prefix.size() && iequals(std::string_view(n).substr(0, prefix.size()), prefix); };
    for (auto* c : m_cvars) {
        if (matches(c->name())) out.push_back(c->name());
    }
    for (auto* c : m_commands) {
        if (matches(c->name())) out.push_back(c->name());
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool CVarRegistry::set(std::string_view name, std::string_view value, CVarSource source) {
    ICVar* c = find(name);
    return c && c->setFromString(value, source);
}

usize CVarRegistry::addChangeListener(ChangeListener fn) {
    std::lock_guard lock(m_listenerMutex);
    m_listeners.emplace_back(++m_nextListenerId, std::make_shared<ChangeListener>(std::move(fn)));
    return m_nextListenerId;
}

void CVarRegistry::removeChangeListener(usize id) {
    std::lock_guard lock(m_listenerMutex);
    std::erase_if(m_listeners, [id](const auto& p) { return p.first == id; });
}

void CVarRegistry::notifyChanged(ICVar& cvar, CVarSource source) {
    std::vector<std::shared_ptr<ChangeListener>> listeners;
    {
        std::lock_guard lock(m_listenerMutex);
        if (m_listeners.empty()) return;
        listeners.reserve(m_listeners.size());
        for (const auto& [id, fn] : m_listeners) listeners.push_back(fn);
    }
    for (const auto& fn : listeners) (*fn)(cvar, source);
}

std::vector<std::string> tokenizeCommandLine(std::string_view line) {
    std::vector<std::string> tokens;
    std::string cur;
    bool inQuotes = false;
    bool have = false;
    for (usize i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '\\' && i + 1 < line.size()) {
            cur += line[++i];
            have = true;
        } else if (c == '"') {
            inQuotes = !inQuotes;
            have = true;
        } else if ((c == ' ' || c == '\t') && !inQuotes) {
            if (have) tokens.push_back(std::move(cur));
            cur.clear();
            have = false;
        } else {
            cur += c;
            have = true;
        }
    }
    if (have) tokens.push_back(std::move(cur));
    return tokens;
}

Result<std::string> CVarRegistry::execute(std::string_view line) {
    auto tokens = tokenizeCommandLine(line);
    if (tokens.empty()) return std::string{};
    if (ICVar* c = find(tokens[0])) {
        if (tokens.size() == 1) {
            return std::format("{} = {} (default {}) - {}", c->name(), c->toString(), c->defaultString(),
                               c->description());
        }
        std::string value = tokens[1];
        for (usize i = 2; i < tokens.size(); ++i) value += " " + tokens[i];
        if (c->hasFlag(CVarFlags::ReadOnly)) return makeError("{} is read-only", c->name());
        if (c->hasFlag(CVarFlags::Cheat) && !cheatsEnabled()) return makeError("{} is a cheat (cheats disabled)", c->name());
        if (!c->setFromString(value, CVarSource::Console)) return makeError("invalid value '{}' for {}", value, c->name());
        std::string out = std::format("{} = {}", c->name(), c->toString());
        if (c->hasFlag(CVarFlags::RequiresRestart)) out += " (restart required)";
        return out;
    }
    if (const ConsoleCommand* cmd = findCommand(tokens[0])) {
        return cmd->invoke(std::span<const std::string>(tokens).subspan(1));
    }
    return makeError("unknown command or cvar '{}'", tokens[0]);
}

nlohmann::json CVarRegistry::saveOverrides(bool allChanged) const {
    nlohmann::json out = nlohmann::json::object();
    for (auto* c : all()) {
        if (c->isDefault()) continue;
        if (!allChanged && !c->hasFlag(CVarFlags::Persist)) continue;
        out[c->name()] = c->toJson();
    }
    std::lock_guard lock(m_mutex);
    // Values for cvars that are not registered in this run are preserved.
    for (const auto& [name, value] : m_pending) {
        if (!out.contains(name)) out[name] = value;
    }
    return out;
}

void CVarRegistry::loadOverrides(const nlohmann::json& overrides) {
    if (!overrides.is_object()) return;
    // Scalability group levels first so explicit per-cvar overrides win.
    auto apply = [&](bool groups) {
        for (const auto& [name, value] : overrides.items()) {
            const bool isGroup = name.starts_with("sg.");
            if (isGroup != groups) continue;
            if (ICVar* c = find(name)) {
                if (!c->setFromJson(value, CVarSource::Config)) {
                    OX_LOG_WARN("cvar", "cannot apply saved value {} to '{}'", value.dump(), name);
                }
            } else {
                std::lock_guard lock(m_mutex);
                std::erase_if(m_pending, [&](const auto& p) { return p.first == name; });
                m_pending.emplace_back(name, value);
            }
        }
    };
    apply(true);
    apply(false);
}

Status CVarRegistry::saveOverridesToFile(const std::string& path, bool allChanged) const {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return makeError("cannot write '{}'", path);
    out << saveOverrides(allChanged).dump(2) << "\n";
    return out ? Status{} : Status(makeError("write failed for '{}'", path));
}

Status CVarRegistry::loadOverridesFromFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) return makeError("cannot open '{}'", path);
    auto j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return makeError("'{}' is not a JSON object", path);
    loadOverrides(j);
    return {};
}

} // namespace ox
