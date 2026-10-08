#include "bindings.hpp"
#include "lua_runtime.hpp"

#include <oxwald/core/assert.hpp>
#include <oxwald/core/cvar.hpp>
#include <oxwald/script/script_events.hpp>
#include <oxwald/script/script_instance.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

namespace ox::script {

namespace {

// Raw C function: builds the message in a luaL_Buffer so no C++ object is alive if tostring raises.
template <log::Level kLevel>
int luaLog(lua_State* L) {
    if (kLevel < log::minLevel()) {
        return 0;
    }
    const int n = lua_gettop(L);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    lua_Debug ar;
    if (lua_getstack(L, 1, &ar) && lua_getinfo(L, "Sl", &ar) && ar.currentline > 0) {
        lua_pushfstring(L, "%s:%d: ", ar.short_src, ar.currentline);
        luaL_addvalue(&b);
    }
    for (int i = 1; i <= n; ++i) {
        if (i > 1) {
            luaL_addchar(&b, ' ');
        }
        luaL_tolstring(L, i, nullptr);
        luaL_addvalue(&b);
    }
    luaL_pushresult(&b);
    size_t len = 0;
    const char* text = lua_tolstring(L, -1, &len);
    LuaRuntime& rt = LuaRuntime::of(L);
    log::write(kLevel, rt.vm ? std::string_view(rt.vm->config().logCategory) : std::string_view("script"),
               std::string_view(text, len));
    return 0;
}

int luaSpawn(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    const auto owner = static_cast<u64>(lua_tointeger(L, lua_upvalueindex(1)));
    return LuaRuntime::of(L).vm->scheduler().spawnFromLua(L, owner);
}

int luaWait(lua_State* L) {
    const lua_Number seconds = luaL_optnumber(L, 1, 0.0);
    if (!LuaRuntime::of(L).vm->scheduler().inTask(L)) {
        return luaL_error(L, "wait() can only be called inside a coroutine started with spawn()");
    }
    lua_settop(L, 0);
    lua_pushnumber(L, seconds);
    return lua_yield(L, 1);
}

constexpr const char* kSandboxBootstrap = R"lua(
local check, allowIo, allowOsExt = ...
local G = _G
local error, next, setmetatable, getmetatable, type, tostring, pairs, ipairs =
      error, next, setmetatable, getmetatable, type, tostring, pairs, ipairs
local pcall_, xpcall_, resume_ = pcall, xpcall, coroutine.resume

local function readonly(t, name)
    return setmetatable({}, {
        __index = t,
        __newindex = function(_, k)
            error("attempt to modify read-only table '" .. name .. "' (key '" .. tostring(k) .. "')", 2)
        end,
        __pairs = function() return next, t, nil end,
        __len = function() return #t end,
        __call = function(_, ...) return t(...) end,
        __metatable = false,
    })
end

local function copy(t)
    local r = {}
    for k, v in pairs(t) do r[k] = v end
    return r
end

local co = copy(coroutine)
co.resume = function(...) return check(resume_(...)) end

local os_ = { clock = os.clock, time = os.time, date = os.date, difftime = os.difftime }
if allowOsExt then
    os_.getenv, os_.remove, os_.rename, os_.tmpname = os.getenv, os.remove, os.rename, os.tmpname
end

local libs = { math = math, string = string, table = table, utf8 = utf8, coroutine = co, os = os_ }
if allowIo then libs.io = io end

local safe = { "assert", "error", "ipairs", "next", "pairs", "rawequal", "rawget", "rawlen", "rawset", "select",
               "setmetatable", "tonumber", "tostring", "type", "_VERSION" }

-- userdata metatables (vec3, quat, ...) are shared by every sandbox: never hand them out.
local function sandboxGetmetatable(o)
    if type(o) == "userdata" then return nil end
    return getmetatable(o)
end

local function build(env, apis)
    for _, k in ipairs(safe) do env[k] = G[k] end
    env.getmetatable = sandboxGetmetatable
    env.pcall = function(...) return check(pcall_(...)) end
    env.xpcall = function(...) return check(xpcall_(...)) end
    for name, lib in pairs(libs) do env[name] = readonly(lib, name) end
    for name, api in pairs(apis) do env[name] = readonly(api, name) end
    env._G = env
    return env
end

return build, readonly
)lua";

std::optional<std::string> readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::filesystem::file_time_type mtimeOf(const std::filesystem::path& path) {
    std::error_code ec;
    auto t = std::filesystem::last_write_time(path, ec);
    return ec ? std::filesystem::file_time_type{} : t;
}

} // namespace

// ---------------------------------------------------------------- construction

static std::unique_ptr<LuaRuntime> makeRuntime(ScriptVM* vm, const ScriptVMConfig& config) {
    auto rt = std::make_unique<LuaRuntime>();
    rt->vm = vm;
    rt->limit = config.memoryLimit;
    rt->instructionLimit = config.instructionLimit;
    rt->hookInterval = std::max(1u, config.hookInterval);
    return rt;
}

ScriptVM::ScriptVM(ScriptVMConfig config)
    : m_config(std::move(config)), m_runtime(makeRuntime(this, m_config)),
      m_lua(sol::default_at_panic, &luaAllocate, m_runtime.get()) {
    m_lua.open_libraries(sol::lib::base, sol::lib::package, sol::lib::coroutine, sol::lib::string, sol::lib::os,
                         sol::lib::math, sol::lib::table, sol::lib::debug, sol::lib::io, sol::lib::utf8);
    lua_State* L = m_lua.lua_state();
    setInstructionLimit(m_config.instructionLimit);

    lua_pushcfunction(L, &luaTraceback);
    m_traceback = sol::reference(L, -1);
    lua_pop(L, 1);

    // Trusted main-state tweaks: hide the shared string metatable and bytecode dumping from everyone.
    m_lua.script("getmetatable('').__metatable = false; string.dump = nil");

    m_events = std::make_unique<ScriptEventBus>(*this);
    m_scheduler = std::make_unique<Scheduler>(*this);
    m_apis = m_lua.create_table();
    m_environments = m_lua.create_table();
    m_environments[sol::metatable_key] = m_lua.create_table_with("__mode", "k");

    installBindings();
    installSandboxBuilder();
}

ScriptVM::~ScriptVM() {
    for (auto& [instance, owner] : m_instances) {
        instance->detach();
    }
    m_instances.clear();
    m_modules.clear();
    m_scheduler->clear();
    m_events->clear();
    m_scheduler.reset();
    m_events.reset();
    m_apis = sol::table();
    m_environments = sol::table();
    m_sandboxBuilder = sol::protected_function();
    m_readonly = sol::protected_function();
    m_traceback = sol::reference();
}

void ScriptVM::setInstructionLimit(u64 limit) {
    m_runtime->instructionLimit = limit;
    lua_State* L = m_lua.lua_state();
    if (limit > 0) {
        lua_sethook(L, &luaCountHook, LUA_MASKCOUNT, static_cast<int>(m_runtime->hookInterval));
    } else {
        lua_sethook(L, nullptr, 0, 0);
    }
}

void ScriptVM::installBindings() {
    bindMath(m_lua);
    for (const char* name : {"vec2", "vec3", "vec4", "quat", "mat4"}) {
        m_apis[name] = m_lua[name];
    }
    bindApi("log", [](sol::state_view, sol::table& api) {
        api["trace"] = &luaLog<log::Level::Trace>;
        api["debug"] = &luaLog<log::Level::Debug>;
        api["info"] = &luaLog<log::Level::Info>;
        api["warn"] = &luaLog<log::Level::Warn>;
        api["error"] = &luaLog<log::Level::Error>;
    });
    // Console variables: cvar.get("r.Bloom"), cvar.set("r.Bloom", false), cvar.getString / exists / reset /
    // description. Changes go through the registry with CVarSource::Console (read-only and cheat flags apply, and
    // the runtime re-applies graphics settings for r.* cvars like for console input).
    bindApi("cvar", [](sol::state_view, sol::table& api) {
        api["get"] = [](sol::this_state ts, const std::string& name) -> sol::object {
            sol::state_view lua(ts);
            const ICVar* c = CVarRegistry::instance().find(name);
            if (!c) {
                return sol::make_object(lua, sol::lua_nil);
            }
            const nlohmann::json j = c->toJson();
            switch (c->type()) {
            case CVarType::Bool: return sol::make_object(lua, j.get<bool>());
            case CVarType::Int: return sol::make_object(lua, j.get<i64>());
            case CVarType::Float: return sol::make_object(lua, j.get<f64>());
            case CVarType::String: return sol::make_object(lua, j.get<std::string>());
            }
            return sol::make_object(lua, sol::lua_nil);
        };
        api["getString"] = [](const std::string& name) -> sol::optional<std::string> {
            const ICVar* c = CVarRegistry::instance().find(name);
            return c ? sol::optional<std::string>(c->toString()) : sol::nullopt;
        };
        api["set"] = [](const std::string& name, const sol::object& value) -> bool {
            std::string text;
            switch (value.get_type()) {
            case sol::type::boolean: text = value.as<bool>() ? "true" : "false"; break;
            case sol::type::number: {
                const f64 d = value.as<f64>();
                text = (std::floor(d) == d && std::abs(d) < 1e15) ? std::to_string(static_cast<i64>(d))
                                                                   : std::format("{}", d);
                break;
            }
            case sol::type::string: text = value.as<std::string>(); break;
            default: return false;
            }
            return CVarRegistry::instance().set(name, text, CVarSource::Console);
        };
        api["exists"] = [](const std::string& name) { return CVarRegistry::instance().find(name) != nullptr; };
        api["reset"] = [](const std::string& name) {
            ICVar* c = CVarRegistry::instance().find(name);
            if (c) {
                c->reset(CVarSource::Console);
            }
            return c != nullptr;
        };
        api["description"] = [](const std::string& name) -> sol::optional<std::string> {
            const ICVar* c = CVarRegistry::instance().find(name);
            return c ? sol::optional<std::string>(c->description()) : sol::nullopt;
        };
    });
}

void ScriptVM::installSandboxBuilder() {
    lua_State* L = m_lua.lua_state();
    if (luaL_loadbufferx(L, kSandboxBootstrap, std::strlen(kSandboxBootstrap), "=sandbox", "t") != LUA_OK) {
        OX_LOG_ERROR("script", "sandbox bootstrap failed to compile: {}", lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }
    lua_pushcfunction(L, &luaCheckExceeded);
    lua_pushboolean(L, m_config.allowIo ? 1 : 0);
    lua_pushboolean(L, m_config.allowOsExtended ? 1 : 0);
    if (lua_pcall(L, 3, 2, 0) != LUA_OK) {
        OX_LOG_ERROR("script", "sandbox bootstrap failed: {}", lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }
    m_sandboxBuilder = sol::protected_function(L, -2);
    m_readonly = sol::protected_function(L, -1);
    lua_pop(L, 2);
}

// ---------------------------------------------------------------- calls

ScriptVM::CallScope::CallScope(ScriptVM& v) : vm(v) {
    if (vm.m_runtime->depth++ == 0) {
        vm.m_runtime->beginTopLevelCall();
    }
}

ScriptVM::CallScope::~CallScope() { --vm.m_runtime->depth; }

ScriptResult ScriptVM::finish(sol::protected_function_result& r, std::string_view context) {
    if (!r.valid()) {
        sol::error err = r;
        return fail(context, err.what());
    }
    ScriptResult out;
    out.ok = true;
    if (r.return_count() > 0) {
        out.value = r.get<sol::object>();
    }
    return out;
}

ScriptResult ScriptVM::fail(std::string_view context, std::string_view message) {
    reportError(context, message);
    ScriptResult out;
    out.error = std::string(message);
    return out;
}

void ScriptVM::reportError(std::string_view context, std::string_view message) {
    ++m_errorCount;
    m_lastError = std::string(message);
    log::write(log::Level::Error, m_config.logCategory, std::format("{}: {}", context, message));
}

ScriptResult ScriptVM::compile(std::string_view code, std::string_view chunkName, sol::protected_function& out) {
    try {
        sol::load_result lr = m_lua.load(code, "@" + std::string(chunkName), sol::load_mode::text);
        if (!lr.valid()) {
            sol::error err = lr;
            return fail(std::format("compile {}", chunkName), err.what());
        }
        out = lr.get<sol::protected_function>();
        return {true, {}, {}};
    } catch (const std::exception& e) {
        return fail(std::format("compile {}", chunkName), e.what());
    }
}

ScriptResult ScriptVM::runString(std::string_view code, std::string_view chunkName, const sol::environment* env) {
    sol::protected_function fn;
    ScriptResult r = compile(code, chunkName, fn);
    if (!r.ok) {
        return r;
    }
    if (env) {
        sol::set_environment(*env, fn);
    }
    return call(fn, chunkName);
}

ScriptResult ScriptVM::runFile(const std::filesystem::path& path, const sol::environment* env) {
    auto source = readFile(path);
    if (!source) {
        return fail(path.generic_string(), "cannot read file");
    }
    return runString(*source, path.generic_string(), env);
}

// ---------------------------------------------------------------- sandboxes

sol::environment ScriptVM::createEnvironment(u64 owner) {
    sol::environment env(m_lua, sol::create);
    populateEnvironment(env, owner);
    m_environments[env] = true; // weak keys: lets bindApi() reach it while it lives
    return env;
}

void ScriptVM::populateEnvironment(sol::environment& env, u64 owner) {
    call(m_sandboxBuilder, "sandbox", env, m_apis);

    env["require"] = [this](const std::string& name) { return requireModule(name); };
    env["print"] = &luaLog<log::Level::Info>;
    env["wait"] = &luaWait;
    env["time"] = [this] { return m_scheduler->time(); };

    lua_State* L = m_lua.lua_state();
    env.push();
    lua_pushinteger(L, static_cast<lua_Integer>(owner));
    lua_pushcclosure(L, &luaSpawn, 1);
    lua_setfield(L, -2, "spawn");
    lua_pop(L, 1);

    sol::table timer = m_lua.create_table();
    timer["after"] = [this, owner](f64 delay, sol::protected_function fn) {
        return m_scheduler->addTimer(delay, 0.0, std::move(fn), owner);
    };
    timer["every"] = [this, owner](f64 interval, sol::protected_function fn) {
        return m_scheduler->addTimer(interval, std::max(interval, 1e-6), std::move(fn), owner);
    };
    timer["cancel"] = [this](u64 id) { m_scheduler->cancel(id); };
    env["timer"] = timer;

    sol::table events = m_lua.create_table();
    events["subscribe"] = [this, owner](const std::string& name, sol::protected_function fn) {
        return m_events->subscribe(
            name,
            [this, fn, name](std::string_view, const sol::object& payload) {
                call(fn, std::format("event '{}'", name), name, payload);
            },
            owner);
    };
    events["unsubscribe"] = [this](u64 id) { m_events->unsubscribe(id); };
    events["publish"] = [this](const std::string& name, sol::object payload) { m_events->publish(name, payload); };
    env["events"] = events;
}

void ScriptVM::releaseOwner(u64 owner) {
    if (owner == 0) {
        return;
    }
    m_scheduler->cancelOwner(owner);
    m_events->unsubscribeOwner(owner);
}

void ScriptVM::bindApi(std::string_view name, const ApiBuilder& builder) {
    sol::table api = m_lua.create_table();
    try {
        builder(sol::state_view(m_lua), api);
    } catch (const std::exception& e) {
        OX_LOG_ERROR("script", "bindApi('{}') failed: {}", name, e.what());
        return;
    }
    const std::string key(name);
    const bool rebinding = hasApi(key);
    m_apis[key] = api;
    // Every live sandbox created earlier (instances, modules, raw createEnvironment() users) gets the API too. A
    // global the script defined itself under that name is kept unless it is the previous version of this API.
    ScriptResult wrapped = call(m_readonly, "bindApi", api, key);
    if (!wrapped.ok) {
        return;
    }
    std::vector<sol::table> envs;
    for (const auto& [env, alive] : m_environments) {
        if (env.get_type() == sol::type::table) {
            envs.push_back(env.as<sol::table>());
        }
    }
    for (sol::table& env : envs) {
        const sol::object existing = env.raw_get<sol::object>(key);
        if (rebinding || !existing.valid() || existing.get_type() == sol::type::lua_nil) {
            env.raw_set(key, wrapped.value);
        }
    }
}

bool ScriptVM::hasApi(std::string_view name) const {
    sol::object o = m_apis[std::string(name)];
    return o.valid() && o.get_type() == sol::type::table;
}

// ---------------------------------------------------------------- modules

void ScriptVM::addSearchRoot(const std::filesystem::path& root) { m_config.searchRoots.push_back(root); }

std::optional<std::filesystem::path> ScriptVM::resolveModule(std::string_view name) const {
    if (name.empty() || name.find("..") != std::string_view::npos || name.front() == '/' ||
        name.find('\\') != std::string_view::npos) {
        return std::nullopt;
    }
    std::string rel(name);
    std::replace(rel.begin(), rel.end(), '.', '/');
    for (const auto& root : m_config.searchRoots) {
        for (const auto& candidate : {root / (rel + ".lua"), root / rel / "init.lua"}) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(candidate, ec)) {
                return candidate;
            }
        }
    }
    return std::nullopt;
}

sol::object ScriptVM::requireModule(const std::string& name) {
    if (auto it = m_modules.find(name); it != m_modules.end()) {
        return it->second.value;
    }
    if (std::find(m_loadingModules.begin(), m_loadingModules.end(), name) != m_loadingModules.end()) {
        throw sol::error(std::format("circular require of module '{}'", name));
    }
    auto path = resolveModule(name);
    if (!path) {
        throw sol::error(std::format("module '{}' not found in script search roots", name));
    }
    auto source = readFile(*path);
    if (!source) {
        throw sol::error(std::format("cannot read module '{}' ({})", name, path->generic_string()));
    }
    sol::environment env = createEnvironment(0);
    m_loadingModules.push_back(name);
    ScriptResult r = runString(*source, path->generic_string(), &env);
    m_loadingModules.pop_back();
    if (!r.ok) {
        throw sol::error(std::format("error while loading module '{}'", name));
    }
    sol::object value = (r.value.valid() && r.value.get_type() != sol::type::lua_nil) ? r.value
                                                                                       : sol::make_object(m_lua, true);
    m_modules[name] = ModuleEntry{value, *path, mtimeOf(*path)};
    return value;
}

// ---------------------------------------------------------------- assets & instances

const ScriptPropertyDesc* ScriptAsset::findProperty(std::string_view name) const {
    for (const auto& p : m_properties) {
        if (p.name == name) {
            return &p;
        }
    }
    return nullptr;
}

std::shared_ptr<ScriptAsset> ScriptVM::loadScript(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        canonical = path;
    }
    std::erase_if(m_assets, [](const std::weak_ptr<ScriptAsset>& w) { return w.expired(); });
    for (auto& weak : m_assets) {
        if (auto a = weak.lock(); a && a->m_path == canonical) {
            return a;
        }
    }
    auto asset = std::make_shared<ScriptAsset>();
    asset->m_path = canonical;
    asset->m_name = canonical.filename().string();
    asset->m_chunkName = canonical.generic_string();
    asset->m_mtime = mtimeOf(canonical);
    m_assets.push_back(asset);
    auto source = readFile(canonical);
    if (!source) {
        reportError(asset->m_chunkName, "cannot read script file");
        return asset;
    }
    asset->m_source = std::move(*source);
    asset->m_version = 1;
    parseProperties(*asset);
    return asset;
}

std::shared_ptr<ScriptAsset> ScriptVM::loadScriptFromString(std::string name, std::string source) {
    auto asset = std::make_shared<ScriptAsset>();
    asset->m_name = name;
    asset->m_chunkName = std::move(name);
    asset->m_source = std::move(source);
    asset->m_version = 1;
    m_assets.push_back(asset);
    parseProperties(*asset);
    return asset;
}

std::unique_ptr<ScriptInstance> ScriptVM::createInstance(std::shared_ptr<ScriptAsset> asset, InstanceInit init) {
    OX_ASSERT(asset != nullptr, "createInstance: null asset");
    return std::unique_ptr<ScriptInstance>(new ScriptInstance(*this, std::move(asset), std::move(init)));
}

void ScriptVM::registerInstance(ScriptInstance* instance) { m_instances[instance] = instance->id(); }
void ScriptVM::unregisterInstance(ScriptInstance* instance) { m_instances.erase(instance); }

static std::optional<ScriptValue> tableToVector(const sol::table& t) {
    const usize n = t.size();
    if (n < 2 || n > 4) {
        return std::nullopt;
    }
    f32 c[4] = {};
    for (usize i = 0; i < n; ++i) {
        sol::object o = t[i + 1];
        if (o.get_type() != sol::type::number) {
            return std::nullopt;
        }
        c[i] = o.as<f32>();
    }
    if (n == 2) return ScriptValue{glm::vec2(c[0], c[1])};
    if (n == 3) return ScriptValue{glm::vec3(c[0], c[1], c[2])};
    return ScriptValue{glm::vec4(c[0], c[1], c[2], c[3])};
}

static std::optional<ScriptPropertyType> inferType(const ScriptValue& v) {
    switch (v.index()) {
    case 1: return ScriptPropertyType::Bool;
    case 2: return ScriptPropertyType::Int;
    case 3: return ScriptPropertyType::Float;
    case 4: return ScriptPropertyType::String;
    case 5: return ScriptPropertyType::Vec2;
    case 6: return ScriptPropertyType::Vec3;
    case 7: return ScriptPropertyType::Vec4;
    default: return std::nullopt;
    }
}

bool ScriptVM::parseProperties(ScriptAsset& asset) {
    const u64 scratchOwner = newOwnerId();
    sol::environment env = createEnvironment(scratchOwner);
    ScriptResult r = runString(asset.m_source, asset.m_chunkName, &env);
    releaseOwner(scratchOwner);
    asset.m_valid = r.ok;
    if (!r.ok) {
        return false;
    }
    std::vector<ScriptPropertyDesc> props;
    sol::object declared = env.raw_get<sol::object>("properties");
    if (declared.get_type() == sol::type::table) {
        for (auto& [key, value] : declared.as<sol::table>()) {
            if (key.get_type() != sol::type::string) {
                continue;
            }
            ScriptPropertyDesc d;
            d.name = key.as<std::string>();
            ScriptValue def;
            std::optional<ScriptPropertyType> type;
            if (value.get_type() == sol::type::table) {
                sol::table t = value.as<sol::table>();
                sol::object defObj = t["default"];
                def = fromLua(defObj);
                if (defObj.get_type() == sol::type::table) {
                    def = tableToVector(defObj.as<sol::table>()).value_or(ScriptValue{});
                }
                if (sol::object typeObj = t["type"]; typeObj.get_type() == sol::type::string) {
                    type = parsePropertyType(typeObj.as<std::string>());
                    if (!type) {
                        OX_LOG_WARN("script", "{}: property '{}' has unknown type '{}'", asset.m_name, d.name,
                                    typeObj.as<std::string>());
                        continue;
                    }
                } else {
                    type = inferType(def);
                }
                if (sol::object o = t["min"]; o.get_type() == sol::type::number) d.min = o.as<f64>();
                if (sol::object o = t["max"]; o.get_type() == sol::type::number) d.max = o.as<f64>();
                if (sol::object o = t["tooltip"]; o.get_type() == sol::type::string) d.tooltip = o.as<std::string>();
                if (sol::object o = t["order"]; o.get_type() == sol::type::number) d.order = o.as<i32>();
            } else {
                def = fromLua(value); // shorthand: speed = 5
                type = inferType(def);
                if (type == ScriptPropertyType::Int) {
                    type = ScriptPropertyType::Float; // `speed = 5` almost always means a number
                }
            }
            if (!type) {
                OX_LOG_WARN("script", "{}: cannot infer type of property '{}'", asset.m_name, d.name);
                continue;
            }
            d.type = *type;
            if (std::holds_alternative<std::monostate>(def)) {
                d.defaultValue = defaultValueFor(d.type);
            } else if (auto coerced = coerceProperty(d, def)) {
                d.defaultValue = *coerced;
            } else {
                OX_LOG_WARN("script", "{}: default of property '{}' does not match type {}", asset.m_name, d.name,
                            toString(d.type));
                d.defaultValue = defaultValueFor(d.type);
            }
            props.push_back(std::move(d));
        }
    }
    std::sort(props.begin(), props.end(), [](const ScriptPropertyDesc& a, const ScriptPropertyDesc& b) {
        return a.order != b.order ? a.order < b.order : a.name < b.name;
    });
    asset.m_properties = std::move(props);
    return true;
}

// ---------------------------------------------------------------- hot reload & update

bool ScriptVM::reloadAsset(ScriptAsset& asset, std::string source) {
    sol::protected_function probe;
    if (!compile(source, asset.m_chunkName, probe).ok) {
        OX_LOG_WARN("script", "hot reload of {} failed to compile, keeping version {}", asset.m_name,
                    asset.m_version);
        return false;
    }
    asset.m_source = std::move(source);
    ++asset.m_version;
    parseProperties(asset);
    std::vector<ScriptInstance*> affected;
    for (auto& [instance, owner] : m_instances) {
        if (instance->m_asset.get() == &asset) {
            affected.push_back(instance);
        }
    }
    for (ScriptInstance* instance : affected) {
        instance->reload();
    }
    OX_LOG_INFO("script", "hot reloaded {} (v{}, {} instance(s))", asset.m_name, asset.m_version, affected.size());
    return true;
}

u32 ScriptVM::pollHotReload() {
    bool modulesChanged = false;
    for (auto it = m_modules.begin(); it != m_modules.end();) {
        if (mtimeOf(it->second.path) != it->second.mtime) {
            OX_LOG_INFO("script", "module '{}' changed", it->first);
            it = m_modules.erase(it);
            modulesChanged = true;
        } else {
            ++it;
        }
    }
    u32 reloaded = 0;
    std::erase_if(m_assets, [](const std::weak_ptr<ScriptAsset>& w) { return w.expired(); });
    auto assets = m_assets; // reloading can load more assets
    for (auto& weak : assets) {
        auto asset = weak.lock();
        if (!asset || asset->m_path.empty()) {
            continue;
        }
        const auto mtime = mtimeOf(asset->m_path);
        if (mtime == asset->m_mtime && !modulesChanged) {
            continue;
        }
        asset->m_mtime = mtime;
        auto source = readFile(asset->m_path);
        if (!source || (*source == asset->m_source && !modulesChanged)) {
            continue;
        }
        if (reloadAsset(*asset, std::move(*source))) {
            ++reloaded;
        }
    }
    return reloaded;
}

void ScriptVM::update(f64 dt) {
    m_scheduler->update(dt);
    if (m_config.hotReloadInterval > 0.0) {
        m_hotReloadAccumulator += dt;
        if (m_hotReloadAccumulator >= m_config.hotReloadInterval) {
            m_hotReloadAccumulator = 0.0;
            pollHotReload();
        }
    }
}

f64 ScriptVM::time() const { return m_scheduler->time(); }

usize ScriptVM::memoryUsed() const { return m_runtime->used; }

// ---------------------------------------------------------------- value conversion

sol::object ScriptVM::toLua(const ScriptValue& value) {
    return std::visit(
        [this](const auto& v) -> sol::object {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                return sol::make_object(m_lua, sol::lua_nil);
            } else {
                return sol::make_object(m_lua, v);
            }
        },
        value);
}

ScriptValue ScriptVM::fromLua(const sol::object& value) {
    switch (value.get_type()) {
    case sol::type::boolean: return value.as<bool>();
    case sol::type::number: {
        lua_State* L = value.lua_state();
        value.push(L);
        const bool isInt = lua_isinteger(L, -1);
        lua_pop(L, 1);
        if (isInt) {
            return value.as<i64>();
        }
        return value.as<f64>();
    }
    case sol::type::string: return value.as<std::string>();
    case sol::type::userdata:
        if (value.is<glm::vec3>()) return value.as<glm::vec3>();
        if (value.is<glm::vec2>()) return value.as<glm::vec2>();
        if (value.is<glm::vec4>()) return value.as<glm::vec4>();
        return {};
    default: return {};
    }
}

} // namespace ox::script
