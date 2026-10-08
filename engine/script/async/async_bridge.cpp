#include <oxwald/script/async_bridge.hpp>
#include <oxwald/script/script_events.hpp>

#include <limits>
#include <tuple>

namespace ox::script {

namespace {

// Builds `await(f)` in the main state: parks the current spawn()ed coroutine (yield math.huge) until the bridge
// wakes it, then returns the value or raises the future's error. Only Lua frames between the coroutine and the
// yield, so it is always yieldable.
constexpr const char* kAwaitSource = R"lua(
local yield, error, type, inf = ...
return function(f)
    if type(f) ~= "userdata" or not f.isReady then
        error("await() expects a Future", 2)
    end
    if not f:isReady() then
        if not f:_park() then
            error("await() can only be called inside a coroutine started with spawn()", 2)
        end
        repeat yield(inf) until f:isReady()
    end
    local ok, value = f:_result()
    if not ok then error(value, 2) end
    return value
end
)lua";

// Runs fn(...) and reports its first result (or the error message) to C++.
constexpr const char* kCallWrapperSource = R"lua(
local pcall, tostring = ...
return function(fn, resolve, reject, ...)
    local ok, result = pcall(fn, ...)
    if ok then resolve(result) else reject(tostring(result)) end
end
)lua";

sol::protected_function compileHelper(ScriptVM& vm, const char* source, const char* name) {
    sol::protected_function chunk;
    ScriptResult r = vm.compile(source, name, chunk);
    OX_ASSERT(r.ok, "async bridge: {} failed to compile: {}", name, r.error);
    return chunk;
}

} // namespace

AsyncBridge::AsyncBridge(ScriptVM& vm) : m_vm(vm), m_wakes(std::make_shared<WakeQueue>()) {
    sol::state& lua = vm.lua();

    lua.new_usertype<LuaFuture>(
        "Future", sol::no_constructor,
        "isReady", [](const LuaFuture& f) { return f.state->ready(); },
        "hasError", [](const LuaFuture& f) { return f.state->hasError(); },
        "error", [](const LuaFuture& f) { return f.state->errorMessage(); },
        "get",
        [](const LuaFuture& f, sol::this_state ts) -> sol::object {
            if (!f.state->ready()) {
                throw std::runtime_error("Future:get(): not ready (use await)");
            }
            if (f.state->hasError()) {
                throw std::runtime_error(f.state->errorMessage());
            }
            return f.extract(sol::state_view(ts));
        },
        "_park", [this](LuaFuture& f, sol::this_state ts) { return park(ts, f); },
        "_result",
        [](const LuaFuture& f, sol::this_state ts) -> std::tuple<bool, sol::object> {
            sol::state_view L(ts);
            if (f.state->hasError()) {
                return {false, sol::make_object(L, f.state->errorMessage())};
            }
            return {true, f.extract(L)};
        });

    sol::protected_function awaitFactory = compileHelper(vm, kAwaitSource, "=async.await");
    sol::object yieldFn = lua["coroutine"]["yield"];
    sol::object errorFn = lua["error"];
    sol::object typeFn = lua["type"];
    ScriptResult made = vm.call(awaitFactory, "async bridge", yieldFn, errorFn, typeFn,
                                std::numeric_limits<f64>::infinity());
    OX_ASSERT(made.ok, "async bridge: await factory failed: {}", made.error);
    sol::protected_function awaitFn = made.value;

    sol::protected_function wrapperFactory = compileHelper(vm, kCallWrapperSource, "=async.call");
    sol::object pcallFn = lua["pcall"];
    sol::object tostringFn = lua["tostring"];
    ScriptResult wrapper = vm.call(wrapperFactory, "async bridge", pcallFn, tostringFn);
    OX_ASSERT(wrapper.ok, "async bridge: call wrapper failed: {}", wrapper.error);
    m_callWrapper = wrapper.value;

    // `await` is a callable API table, so every sandbox (also ones created earlier) gets it as a read-only global.
    // __call is a pure Lua function: no C++ frame between the coroutine and coroutine.yield (stays yieldable).
    sol::protected_function trampolineFactory =
        compileHelper(vm, "local awaitFn = ...; return function(_, f) return awaitFn(f) end", "=async.call_await");
    ScriptResult trampoline = vm.call(trampolineFactory, "async bridge", awaitFn);
    OX_ASSERT(trampoline.ok, "async bridge: await trampoline failed: {}", trampoline.error);
    sol::table mt = lua.create_table();
    mt["__call"] = trampoline.value;
    vm.bindApi("await", [mt](sol::state_view, sol::table& api) { api[sol::metatable_key] = mt; });
    vm.bindApi("async", [awaitFn](sol::state_view, sol::table& api) { api["await"] = awaitFn; });

    m_env = vm.createEnvironment();
}

AsyncBridge::~AsyncBridge() = default;

bool AsyncBridge::park(sol::this_state ts, LuaFuture& f) {
    Scheduler& sched = m_vm.scheduler();
    if (!sched.inTask(ts.lua_state())) {
        return false;
    }
    const u64 id = sched.currentTaskId();
    std::weak_ptr<WakeQueue> weak = m_wakes;
    // Completion may happen on any thread; the wake itself is applied on the main thread in update().
    f.state->addContinuation([weak, id] {
        if (auto q = weak.lock()) {
            std::lock_guard lock(q->mutex);
            q->ids.push_back(id);
        }
    });
    return true; // if it completed meanwhile, the caller's loop sees isReady() and does not yield
}

void AsyncBridge::update() {
    std::vector<u64> ids;
    {
        std::lock_guard lock(m_wakes->mutex);
        ids.swap(m_wakes->ids);
    }
    for (u64 id : ids) {
        m_vm.scheduler().wake(id);
    }
}

usize AsyncBridge::pendingWakeups() const {
    std::lock_guard lock(m_wakes->mutex);
    return m_wakes->ids.size();
}

std::pair<sol::object, sol::object> AsyncBridge::makeResolvers(
    const std::shared_ptr<ox::Promise<sol::main_object>>& promise) {
    sol::state& lua = m_vm.lua();
    // sol::main_object anchors the value in the main thread, so it outlives the (collected) coroutine thread.
    sol::object resolve = sol::make_object(lua, [promise](sol::main_object value) { promise->setValue(std::move(value)); });
    sol::object reject = sol::make_object(lua, [promise](const std::string& message) { promise->setError(message); });
    return {resolve, reject};
}

} // namespace ox::script
