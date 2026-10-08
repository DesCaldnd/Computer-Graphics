#pragma once

#include <oxwald/core/result.hpp>
#include <oxwald/core/types.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// Console variables. Declare them as (static) objects in a translation unit that is linked into the program
// (i.e. next to the code that reads them) — a CVar in an otherwise unreferenced .cpp of a static library is
// dropped by the linker and never registers.
//
//   static ox::CVar<bool> cvVsync("r.VSync", true, "Vertical sync", ox::CVarFlags::Persist);
//   static ox::CVar<int> cvShadowRes("r.Shadows.Resolution", 2048, "Shadow map resolution",
//                                    ox::Scalability::Shadows, {512, 1024, 2048, 4096});
//   static ox::CVar<int> cvUpscaler("r.Upscaler", 0, "Upscaler", ox::CVarEnum{"Off", "FSR1", "DLSS"});
//   if (cvVsync) ...;  cvShadowRes.get();  cvShadowRes.onChanged([](int now, int before) { ... });
namespace ox {

enum class CVarFlags : u32 {
    None = 0,
    ReadOnly = 1u << 0,        // only code may change it (console/config/scalability rejected)
    Persist = 1u << 1,         // saved by CVarRegistry::saveOverrides
    RequiresRestart = 1u << 2, // change is stored but only takes effect after restart
    Cheat = 1u << 3,           // console changes need CVarRegistry::setCheatsEnabled(true)
};
[[nodiscard]] constexpr CVarFlags operator|(CVarFlags a, CVarFlags b) { return CVarFlags(u32(a) | u32(b)); }
[[nodiscard]] constexpr CVarFlags operator&(CVarFlags a, CVarFlags b) { return CVarFlags(u32(a) & u32(b)); }
[[nodiscard]] constexpr bool hasFlag(CVarFlags set, CVarFlags f) { return (u32(set) & u32(f)) != 0; }

enum class CVarType : u8 { Bool, Int, Float, String };

// Who changed a value; used for permission checks and to tell user overrides from scalability presets.
enum class CVarSource : u8 { Default, Code, Scalability, Config, Console };

// Scalability groups (see scalability.hpp for levels and presets).
enum class Scalability : u8 {
    ViewDistance,
    AntiAliasing,
    Shadows,
    GlobalIllumination,
    Reflections,
    PostProcess,
    Textures,
    Effects,
    Foliage,
    Shading,
    Volumetrics,
    RayTracing,
    Count,
    None = 0xFF,
};
inline constexpr usize kScalabilityGroupCount = static_cast<usize>(Scalability::Count);

enum class QualityLevel : i32 { Custom = -1, Low = 0, Medium = 1, High = 2, Ultra = 3 };
inline constexpr usize kQualityLevelCount = 4;

// Names for an int cvar used as an enum: setFromString accepts names or numbers, toString prints the name.
struct CVarEnum {
    std::vector<std::string> names;
    // Extra spellings accepted when parsing (e.g. "DLAA" for "Native"); toString always prints `names`.
    std::vector<std::pair<std::string, int>> aliases;
    CVarEnum(std::initializer_list<std::string> n) : names(n) {}
    CVarEnum&& alias(std::string name, int value) && {
        aliases.emplace_back(std::move(name), value);
        return std::move(*this);
    }
};

class ICVar {
public:
    ICVar(const ICVar&) = delete;
    ICVar& operator=(const ICVar&) = delete;
    virtual ~ICVar();

    [[nodiscard]] const std::string& name() const { return m_name; }
    [[nodiscard]] const std::string& description() const { return m_description; }
    [[nodiscard]] CVarFlags flags() const { return m_flags; }
    [[nodiscard]] bool hasFlag(CVarFlags f) const { return ox::hasFlag(m_flags, f); }
    [[nodiscard]] Scalability group() const { return m_group; }
    [[nodiscard]] const std::vector<std::string>& enumNames() const { return m_enumNames; }
    [[nodiscard]] const std::vector<std::pair<std::string, int>>& enumAliases() const { return m_enumAliases; }
    [[nodiscard]] CVarSource lastSource() const { return m_lastSource.load(std::memory_order_relaxed); }
    [[nodiscard]] std::optional<double> minValue() const { return m_min; }
    [[nodiscard]] std::optional<double> maxValue() const { return m_max; }

    [[nodiscard]] virtual CVarType type() const = 0;
    [[nodiscard]] virtual std::string toString() const = 0;
    [[nodiscard]] virtual std::string defaultString() const = 0;
    [[nodiscard]] virtual nlohmann::json toJson() const = 0;
    // Parse + permission check + clamp. Returns false on parse error or when the source may not change it.
    virtual bool setFromString(std::string_view text, CVarSource source) = 0;
    virtual bool setFromJson(const nlohmann::json& value, CVarSource source) = 0;
    virtual void reset(CVarSource source = CVarSource::Code) = 0;
    [[nodiscard]] virtual bool isDefault() const = 0;

    // Scalability binding (no-ops when the cvar has no per-level values).
    [[nodiscard]] virtual bool hasLevelValues() const = 0;
    virtual void applyLevel(QualityLevel level) = 0;
    [[nodiscard]] virtual bool matchesLevel(QualityLevel level) const = 0;
    [[nodiscard]] virtual std::string levelString(QualityLevel level) const = 0;

protected:
    ICVar(std::string_view name, std::string_view description, CVarFlags flags, Scalability group,
          std::vector<std::string> enumNames, std::optional<double> min, std::optional<double> max);
    // Registers with the registry; called at the end of the derived constructor so the object is complete.
    void registerSelf();
    [[nodiscard]] bool allowed(CVarSource source) const;
    void noteChanged(CVarSource source);

    std::string m_name;
    std::string m_description;
    CVarFlags m_flags;
    Scalability m_group;
    std::vector<std::string> m_enumNames;
    std::vector<std::pair<std::string, int>> m_enumAliases;
    std::optional<double> m_min;
    std::optional<double> m_max;
    std::atomic<CVarSource> m_lastSource{CVarSource::Default};
    bool m_registered = false;
};

namespace detail {
std::optional<bool> parseCVarBool(std::string_view text);
std::optional<int> parseCVarInt(std::string_view text, const std::vector<std::string>& enumNames,
                                const std::vector<std::pair<std::string, int>>& aliases = {});
std::optional<float> parseCVarFloat(std::string_view text);
std::string formatCVarFloat(float v);
} // namespace detail

template <class T>
concept CVarValue = std::is_same_v<T, bool> || std::is_same_v<T, int> || std::is_same_v<T, float> ||
                    std::is_same_v<T, std::string>;

template <CVarValue T>
class CVar final : public ICVar {
public:
    using ChangeFn = std::function<void(const T& newValue, const T& oldValue)>;

    CVar(std::string_view name, T defaultValue, std::string_view description, CVarFlags flags = CVarFlags::None)
        : ICVar(name, description, flags, Scalability::None, {}, std::nullopt, std::nullopt),
          m_default(defaultValue), m_value(defaultValue) {
        registerSelf();
    }

    // Numeric with clamping range.
    CVar(std::string_view name, T defaultValue, std::string_view description, T minValue, T maxValue,
         CVarFlags flags = CVarFlags::None)
        requires(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
        : ICVar(name, description, flags, Scalability::None, {}, double(minValue), double(maxValue)),
          m_default(defaultValue), m_value(defaultValue) {
        registerSelf();
    }

    // Enum-as-int.
    CVar(std::string_view name, T defaultValue, std::string_view description, CVarEnum names,
         CVarFlags flags = CVarFlags::None)
        requires std::is_same_v<T, int>
        : ICVar(name, description, flags, Scalability::None, names.names, 0.0,
                double(std::max<usize>(names.names.size(), 1) - 1)),
          m_default(defaultValue), m_value(defaultValue) {
        m_enumAliases = std::move(names.aliases);
        registerSelf();
    }

    // Bound to a scalability group with values for Low, Medium, High, Ultra.
    CVar(std::string_view name, T defaultValue, std::string_view description, Scalability group,
         std::initializer_list<T> levelValues, CVarFlags flags = CVarFlags::None)
        : ICVar(name, description, flags, group, {}, std::nullopt, std::nullopt), m_default(defaultValue),
          m_value(defaultValue) {
        OX_ASSERT(levelValues.size() == kQualityLevelCount, "cvar {} needs {} scalability values", name,
                  kQualityLevelCount);
        m_levels.emplace();
        std::copy(levelValues.begin(), levelValues.end(), m_levels->begin());
        registerSelf();
    }

    ~CVar() override = default;

    [[nodiscard]] T get() const {
        if constexpr (std::is_same_v<T, std::string>) {
            std::lock_guard lock(m_mutex);
            return m_value;
        } else {
            return m_value.load(std::memory_order_relaxed);
        }
    }
    operator T() const { return get(); } // NOLINT(google-explicit-constructor)
    [[nodiscard]] const T& defaultValue() const { return m_default; }

    // Code path: always allowed (even ReadOnly). Returns true when the value changed.
    bool set(const T& value, CVarSource source = CVarSource::Code) {
        if (!allowed(source)) return false;
        return store(value, source);
    }

    // Callback on every change (called on the thread that changed the value). Returns an id for removal.
    usize onChanged(ChangeFn fn) {
        std::lock_guard lock(m_callbackMutex);
        m_callbacks.emplace_back(++m_nextCallbackId, std::move(fn));
        return m_nextCallbackId;
    }
    void removeCallback(usize id) {
        std::lock_guard lock(m_callbackMutex);
        std::erase_if(m_callbacks, [id](const auto& p) { return p.first == id; });
    }

    [[nodiscard]] CVarType type() const override {
        if constexpr (std::is_same_v<T, bool>) return CVarType::Bool;
        else if constexpr (std::is_same_v<T, int>) return CVarType::Int;
        else if constexpr (std::is_same_v<T, float>) return CVarType::Float;
        else return CVarType::String;
    }
    [[nodiscard]] std::string toString() const override { return format(get()); }
    [[nodiscard]] std::string defaultString() const override { return format(m_default); }
    [[nodiscard]] nlohmann::json toJson() const override { return nlohmann::json(get()); }

    bool setFromString(std::string_view text, CVarSource source) override {
        auto v = parse(text);
        if (!v || !allowed(source)) return false;
        store(*v, source);
        return true;
    }
    bool setFromJson(const nlohmann::json& j, CVarSource source) override {
        std::optional<T> v;
        if constexpr (std::is_same_v<T, bool>) {
            if (j.is_boolean()) v = j.get<bool>();
            else if (j.is_number()) v = j.get<double>() != 0.0;
        } else if constexpr (std::is_same_v<T, std::string>) {
            if (j.is_string()) v = j.get<std::string>();
        } else {
            if (j.is_number()) v = static_cast<T>(j.get<double>());
            else if (j.is_boolean()) v = static_cast<T>(j.get<bool>());
        }
        if (!v && j.is_string()) v = parse(j.get<std::string>());
        if (!v || !allowed(source)) return false;
        store(*v, source);
        return true;
    }
    void reset(CVarSource source = CVarSource::Code) override {
        if (allowed(source)) store(m_default, source);
    }
    [[nodiscard]] bool isDefault() const override { return get() == m_default; }

    [[nodiscard]] bool hasLevelValues() const override { return m_levels.has_value(); }
    void applyLevel(QualityLevel level) override {
        if (!m_levels || level == QualityLevel::Custom) return;
        if (hasFlag(CVarFlags::ReadOnly)) return;
        store((*m_levels)[usize(level)], CVarSource::Scalability);
    }
    [[nodiscard]] bool matchesLevel(QualityLevel level) const override {
        return m_levels && level != QualityLevel::Custom && get() == (*m_levels)[usize(level)];
    }
    [[nodiscard]] std::string levelString(QualityLevel level) const override {
        return m_levels && level != QualityLevel::Custom ? format((*m_levels)[usize(level)]) : std::string{};
    }
    [[nodiscard]] std::optional<T> levelValue(QualityLevel level) const {
        if (!m_levels || level == QualityLevel::Custom) return std::nullopt;
        return (*m_levels)[usize(level)];
    }

private:
    T clamp(T v) const {
        if constexpr (std::is_arithmetic_v<T> && !std::is_same_v<T, bool>) {
            if (m_min && v < static_cast<T>(*m_min)) v = static_cast<T>(*m_min);
            if (m_max && v > static_cast<T>(*m_max)) v = static_cast<T>(*m_max);
        }
        return v;
    }

    bool store(T value, CVarSource source) {
        value = clamp(std::move(value));
        T old;
        if constexpr (std::is_same_v<T, std::string>) {
            std::lock_guard lock(m_mutex);
            old = m_value;
            m_value = value;
        } else {
            old = m_value.exchange(value, std::memory_order_relaxed);
        }
        if (old == value) {
            m_lastSource.store(source, std::memory_order_relaxed);
            return false;
        }
        noteChanged(source);
        std::vector<ChangeFn> callbacks;
        {
            std::lock_guard lock(m_callbackMutex);
            for (const auto& [id, fn] : m_callbacks) callbacks.push_back(fn);
        }
        for (const auto& fn : callbacks) fn(value, old);
        return true;
    }

    std::string format(const T& v) const {
        if constexpr (std::is_same_v<T, bool>) {
            return v ? "true" : "false";
        } else if constexpr (std::is_same_v<T, int>) {
            if (v >= 0 && usize(v) < m_enumNames.size()) return m_enumNames[usize(v)];
            return std::to_string(v);
        } else if constexpr (std::is_same_v<T, float>) {
            return detail::formatCVarFloat(v);
        } else {
            return v;
        }
    }

    std::optional<T> parse(std::string_view text) const {
        if constexpr (std::is_same_v<T, bool>) return detail::parseCVarBool(text);
        else if constexpr (std::is_same_v<T, int>) return detail::parseCVarInt(text, m_enumNames, m_enumAliases);
        else if constexpr (std::is_same_v<T, float>) return detail::parseCVarFloat(text);
        else return std::string(text);
    }

    T m_default;
    std::conditional_t<std::is_same_v<T, std::string>, std::string, std::atomic<T>> m_value;
    mutable std::mutex m_mutex;
    std::optional<std::array<T, kQualityLevelCount>> m_levels;
    std::mutex m_callbackMutex;
    std::vector<std::pair<usize, ChangeFn>> m_callbacks;
    usize m_nextCallbackId = 0;
};

extern template class CVar<bool>;
extern template class CVar<int>;
extern template class CVar<float>;
extern template class CVar<std::string>;

// Console command: `ox::ConsoleCommand cmdReload("r.ReloadShaders", "Recompile shaders", [](auto args) { ... });`
class ConsoleCommand {
public:
    // Args exclude the command name. The returned string is printed by the console.
    using Fn = std::function<std::string(std::span<const std::string> args)>;

    ConsoleCommand(std::string_view name, std::string_view description, Fn fn);
    ~ConsoleCommand();
    ConsoleCommand(const ConsoleCommand&) = delete;
    ConsoleCommand& operator=(const ConsoleCommand&) = delete;

    [[nodiscard]] const std::string& name() const { return m_name; }
    [[nodiscard]] const std::string& description() const { return m_description; }
    std::string invoke(std::span<const std::string> args) const { return m_fn(args); }

private:
    std::string m_name;
    std::string m_description;
    Fn m_fn;
};

// Process-wide registry of cvars and console commands (one of the few allowed global registries).
class CVarRegistry {
public:
    static CVarRegistry& instance();

    [[nodiscard]] ICVar* find(std::string_view name) const;
    template <class T>
    [[nodiscard]] CVar<T>* findAs(std::string_view name) const {
        return dynamic_cast<CVar<T>*>(find(name));
    }
    [[nodiscard]] const ConsoleCommand* findCommand(std::string_view name) const;
    // Sorted by name.
    [[nodiscard]] std::vector<ICVar*> all() const;
    [[nodiscard]] std::vector<ICVar*> inGroup(Scalability group) const;
    [[nodiscard]] std::vector<std::string> complete(std::string_view prefix) const;

    bool set(std::string_view name, std::string_view value, CVarSource source = CVarSource::Console);

    // Console line: "name" prints the value, "name value" sets it, otherwise runs a command.
    // Quotes group arguments: r.Foo "a b".
    Result<std::string> execute(std::string_view line);

    // Called after any registered cvar changed value (from code, console, config or a scalability preset), on the
    // thread that changed it. Lets subsystems react to `set()`/`execute()`/Lua changes, e.g. the runtime re-applies
    // graphics settings when an r.* cvar changes from the console. Returns an id for removeChangeListener().
    using ChangeListener = std::function<void(ICVar& cvar, CVarSource source)>;
    usize addChangeListener(ChangeListener fn);
    void removeChangeListener(usize id);

    void setCheatsEnabled(bool enabled) { m_cheats.store(enabled); }
    [[nodiscard]] bool cheatsEnabled() const { return m_cheats.load(); }
    // Set when a RequiresRestart cvar changed since startup.
    [[nodiscard]] bool restartRequired() const { return m_restartRequired.load(); }
    void clearRestartRequired() { m_restartRequired.store(false); }

    // {"name": value} for every Persist cvar that differs from its default (or all non-default with allChanged).
    [[nodiscard]] nlohmann::json saveOverrides(bool allChanged = false) const;
    // Applies values with CVarSource::Config. Unknown names are kept and applied when the cvar registers later.
    void loadOverrides(const nlohmann::json& overrides);
    Status saveOverridesToFile(const std::string& path, bool allChanged = false) const;
    Status loadOverridesFromFile(const std::string& path);

    // Internal (ICVar / ConsoleCommand lifetime).
    void add(ICVar& cvar);
    void remove(ICVar& cvar);
    void addCommand(ConsoleCommand& cmd);
    void removeCommand(ConsoleCommand& cmd);
    void markRestartRequired() { m_restartRequired.store(true); }
    void notifyChanged(ICVar& cvar, CVarSource source);

private:
    CVarRegistry() = default;

    mutable std::recursive_mutex m_mutex;
    std::vector<ICVar*> m_cvars;
    std::vector<ConsoleCommand*> m_commands;
    std::vector<std::pair<std::string, nlohmann::json>> m_pending;
    std::atomic<bool> m_cheats{false};
    std::atomic<bool> m_restartRequired{false};
    std::mutex m_listenerMutex;
    std::vector<std::pair<usize, std::shared_ptr<ChangeListener>>> m_listeners;
    usize m_nextListenerId = 0;
};

// Splits a console line into tokens honouring double quotes and backslash escapes.
[[nodiscard]] std::vector<std::string> tokenizeCommandLine(std::string_view line);

} // namespace ox
