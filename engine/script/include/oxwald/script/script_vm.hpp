#pragma once

#include <oxwald/core/log.hpp>
#include <oxwald/core/types.hpp>
#include <oxwald/script/script_value.hpp>
#include <oxwald/script/sol_glm.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ox::script {

class ScriptAsset;
class ScriptInstance;
class ScriptEventBus;
class Scheduler;
struct LuaRuntime;

struct ScriptVMConfig {
    usize memoryLimit = 256ull << 20;  // bytes for the whole VM; 0 = unlimited
    u64 instructionLimit = 50'000'000; // per entry call from C++ (lifecycle call, event, coroutine resume); 0 = off
    u32 hookInterval = 1000;           // instructions between limit checks
    bool allowIo = false;              // expose the `io` library (will be replaced by a VFS-restricted API)
    bool allowOsExtended = false;      // os.getenv/remove/rename/tmpname; os.execute/exit are never exposed
    f64 hotReloadInterval = 0.5;       // update() polls file mtimes this often; <= 0 disables
    std::vector<std::filesystem::path> searchRoots; // `require("a.b")` -> <root>/a/b.lua or <root>/a/b/init.lua
    std::string logCategory = "script";
};

struct ScriptResult {
    bool ok = false;
    std::string error; // message with chunk:line and traceback
    sol::object value; // first return value
    explicit operator bool() const { return ok; }
};

// Owns the Lua state. Every script instance runs in its own sandboxed environment; all calls from C++ go through
// call()/run*() which catch errors (logged with traceback) and enforce the instruction budget. Not thread-safe.
// Must outlive all ScriptInstances and ScriptAssets created from it.
class ScriptVM {
public:
    explicit ScriptVM(ScriptVMConfig config = {});
    ~ScriptVM();
    ScriptVM(const ScriptVM&) = delete;
    ScriptVM& operator=(const ScriptVM&) = delete;

    sol::state& lua() { return m_lua; }
    const ScriptVMConfig& config() const { return m_config; }
    void setInstructionLimit(u64 limit);

    // Fresh sandbox. `owner` tags coroutines/timers/event subscriptions created from it (released by
    // releaseOwner()); 0 = unowned.
    sol::environment createEnvironment(u64 owner = 0);
    void releaseOwner(u64 owner);
    u64 newOwnerId() { return ++m_nextOwner; }

    ScriptResult runString(std::string_view code, std::string_view chunkName, const sol::environment* env = nullptr);
    ScriptResult runFile(const std::filesystem::path& path, const sol::environment* env = nullptr);
    // Compiles (text only, never bytecode) without running.
    ScriptResult compile(std::string_view code, std::string_view chunkName, sol::protected_function& out);

    template <class... Args>
    ScriptResult call(const sol::protected_function& fn, std::string_view context, Args&&... args) {
        if (!fn.valid()) {
            return {};
        }
        CallScope scope(*this);
        try {
            sol::protected_function pf = fn;
            pf.set_error_handler(m_traceback);
            sol::protected_function_result r = pf(std::forward<Args>(args)...);
            return finish(r, context);
        } catch (const std::exception& e) {
            return fail(context, e.what());
        }
    }

    // Module search roots for `require`.
    void addSearchRoot(const std::filesystem::path& root);
    std::optional<std::filesystem::path> resolveModule(std::string_view name) const;

    // Extension point for other modules: builds a table once; every sandbox sees it as a read-only global `name`.
    //   vm.bindApi("physics", [&](sol::state_view lua, sol::table& api) {
    //       api["raycast"] = [&world](glm::vec3 from, glm::vec3 dir, f32 dist) { ... };
    //   });
    using ApiBuilder = std::function<void(sol::state_view lua, sol::table& api)>;
    void bindApi(std::string_view name, const ApiBuilder& builder);
    bool hasApi(std::string_view name) const;

    // Script assets (cached by path) and instances.
    std::shared_ptr<ScriptAsset> loadScript(const std::filesystem::path& path);
    std::shared_ptr<ScriptAsset> loadScriptFromString(std::string name, std::string source);
    // `init` runs before onCreate, e.g. to put the entity handle into `self` (ECS integration).
    using InstanceInit = std::function<void(ScriptInstance&, sol::table& self)>;
    std::unique_ptr<ScriptInstance> createInstance(std::shared_ptr<ScriptAsset> asset, InstanceInit init = {});

    // Advances script time, runs due timers and coroutines, polls hot reload every hotReloadInterval.
    void update(f64 dt);
    f64 time() const;
    // Re-runs changed script files/modules in every live instance; returns the number of assets reloaded.
    u32 pollHotReload();
    // Hot reload of a script whose source does not come from a file (asset database, network): swaps the source of
    // `asset` and reloads every live instance in place (same self table, on_reload). Keeps the old version and
    // returns false when the new source does not compile.
    bool reloadScript(ScriptAsset& asset, std::string source) { return reloadAsset(asset, std::move(source)); }

    ScriptEventBus& events() { return *m_events; }
    Scheduler& scheduler() { return *m_scheduler; }

    usize memoryUsed() const;
    u64 errorCount() const { return m_errorCount; }
    const std::string& lastError() const { return m_lastError; }

    // Converters between ScriptValue and Lua (glm vectors become vec2/vec3/vec4 userdata).
    sol::object toLua(const ScriptValue& value);
    static ScriptValue fromLua(const sol::object& value);

    // Logs a script error (used by instances/scheduler/events too).
    void reportError(std::string_view context, std::string_view message);

private:
    friend class ScriptInstance;
    friend class Scheduler;

    struct CallScope {
        explicit CallScope(ScriptVM& vm);
        ~CallScope();
        ScriptVM& vm;
    };

    ScriptResult finish(sol::protected_function_result& r, std::string_view context);
    ScriptResult fail(std::string_view context, std::string_view message);
    void installBindings();
    void installSandboxBuilder();
    void populateEnvironment(sol::environment& env, u64 owner);
    sol::object requireModule(const std::string& name);
    void registerInstance(ScriptInstance* instance);
    void unregisterInstance(ScriptInstance* instance);
    bool parseProperties(ScriptAsset& asset);
    bool reloadAsset(ScriptAsset& asset, std::string source);

    ScriptVMConfig m_config;
    std::unique_ptr<LuaRuntime> m_runtime; // allocator + hook state, must outlive m_lua
    sol::state m_lua;
    sol::reference m_traceback; // C message handler (adds traceback; runs no Lua code)
    sol::protected_function m_sandboxBuilder;
    sol::protected_function m_readonly;
    sol::table m_apis;
    std::unique_ptr<ScriptEventBus> m_events;
    std::unique_ptr<Scheduler> m_scheduler;

    struct ModuleEntry {
        sol::object value;
        std::filesystem::path path;
        std::filesystem::file_time_type mtime;
    };
    std::unordered_map<std::string, ModuleEntry> m_modules;
    std::vector<std::string> m_loadingModules; // cycle detection

    std::vector<std::weak_ptr<ScriptAsset>> m_assets;
    std::unordered_map<ScriptInstance*, u64> m_instances;
    u64 m_nextOwner = 0;
    u64 m_errorCount = 0;
    std::string m_lastError;
    f64 m_hotReloadAccumulator = 0.0;
};

} // namespace ox::script
