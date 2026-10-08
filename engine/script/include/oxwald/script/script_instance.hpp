#pragma once

#include <oxwald/script/script_vm.hpp>

#include <filesystem>
#include <unordered_map>

namespace ox::script {

// Source + metadata of one script file. Shared by all instances running it; hot reload swaps the source and
// bumps version().
class ScriptAsset {
public:
    const std::string& name() const { return m_name; }
    const std::filesystem::path& path() const { return m_path; } // empty for string scripts
    const std::string& source() const { return m_source; }
    u32 version() const { return m_version; }
    bool valid() const { return m_valid; } // compiled and the top level ran without error

    // Declared `properties = { ... }`, sorted by (order, name). Available without creating an instance.
    const std::vector<ScriptPropertyDesc>& properties() const { return m_properties; }
    const ScriptPropertyDesc* findProperty(std::string_view name) const;

private:
    friend class ScriptVM;
    friend class ScriptInstance;
    std::string m_name;
    std::string m_chunkName;
    std::filesystem::path m_path;
    std::filesystem::file_time_type m_mtime{};
    std::string m_source;
    u32 m_version = 0;
    bool m_valid = false;
    std::vector<ScriptPropertyDesc> m_properties;
};

// One running copy of a script (one per entity in the ECS integration).
//
// Script side:
//   properties = { speed = { type = "float", default = 5, min = 0, max = 20 } }
//   function onCreate(self) end            -- self: per-instance state table, survives hot reload
//   function onStart(self) end             -- before the first update
//   function onUpdate(self, dt) end
//   function onFixedUpdate(self, dt) end
//   function onDestroy(self) end
//   function onEvent(self, name, payload) end
//   function on_reload(self) end           -- after hot reload (onReload also accepted)
//
// Errors never propagate: they are logged and counted; the instance keeps running.
class ScriptInstance {
public:
    enum class State : u8 { Uninitialized, Created, Started, Destroyed };

    ~ScriptInstance();
    ScriptInstance(const ScriptInstance&) = delete;
    ScriptInstance& operator=(const ScriptInstance&) = delete;

    u64 id() const { return m_owner; }
    State state() const { return m_state; }
    const std::shared_ptr<ScriptAsset>& asset() const { return m_asset; }
    u32 loadedVersion() const { return m_loadedVersion; }
    sol::table& self() { return m_self; }
    sol::environment& environment() { return m_env; }
    u32 errorCount() const { return m_errors; }

    // Per-instance overrides of declared properties (editor/prefab values). Values are coerced/clamped to the
    // declaration; returns false for unknown properties or incompatible types. Applied immediately if created.
    bool setProperty(std::string_view name, const ScriptValue& value);
    ScriptValue getProperty(std::string_view name) const; // current value in self (or override/default)
    const std::unordered_map<std::string, ScriptValue>& overrides() const { return m_overrides; }

    // Lifecycle (driven by the ECS script system later).
    bool create();
    void start();
    void update(f32 dt);       // calls start() first if needed
    void fixedUpdate(f32 dt);  // idem
    void destroy();
    void sendEvent(std::string_view name, const sol::object& payload);
    void sendEvent(std::string_view name, const ScriptValue& payload = {});

    // Hot reload: fresh environment with the new chunk, same self table, new property defaults, on_reload(self).
    bool reload();

    // Calls any global function of the script: fn(self, args...).
    template <class... Args>
    ScriptResult invoke(std::string_view function, Args&&... args) {
        if (!m_vm || m_state == State::Uninitialized || m_state == State::Destroyed) {
            return {};
        }
        sol::object fn = m_env[std::string(function)];
        if (fn.get_type() != sol::type::function) {
            return {true, {}, {}};
        }
        ScriptResult r = m_vm->call(fn.as<sol::protected_function>(), contextFor(function), m_self,
                                    std::forward<Args>(args)...);
        if (!r.ok) {
            ++m_errors;
        }
        return r;
    }

private:
    friend class ScriptVM;
    ScriptInstance(ScriptVM& vm, std::shared_ptr<ScriptAsset> asset, ScriptVM::InstanceInit init);
    bool loadChunk(sol::environment& env);
    void applyProperties(bool onlyMissing);
    std::string contextFor(std::string_view function) const;
    void detach(); // VM is going away

    ScriptVM* m_vm;
    std::shared_ptr<ScriptAsset> m_asset;
    ScriptVM::InstanceInit m_init;
    u64 m_owner;
    State m_state = State::Uninitialized;
    sol::environment m_env;
    sol::table m_self;
    std::unordered_map<std::string, ScriptValue> m_overrides;
    u32 m_loadedVersion = 0;
    u32 m_errors = 0;
};

} // namespace ox::script
