#pragma once

// Bridge between ox::Future (async module) and the Lua script scheduler. Only available when the async module is
// configured (compile definition OX_SCRIPT_HAS_ASYNC=1, link Oxwald::script).
//
// Lua side (inside spawn()ed coroutines):
//   local mesh = await(assets.load("props/door.glb"))   -- parks the coroutine until the C++ future completes;
//                                                         -- a failed future raises a Lua error (use pcall)
//   local f = assets.load("x"); if f:isReady() then ... end   -- Future: isReady() hasError() error() get()
//
// C++ side:
//   AsyncBridge bridge(vm);
//   vm.bindApi("assets", [&](sol::state_view, sol::table& api) {
//       api["load"] = [&](std::string path) { return bridge.wrap(assetDb.loadAsync(path)); };   // Future<T> -> Lua
//   });
//   per frame (main thread): bridge.update(); vm.update(dt);
//   // C++ coroutine awaiting a Lua function / script method (runs as a script coroutine, may wait()/await()):
//   sol::main_object r = co_await bridge.call(fn, 1, 2);
//   sol::main_object v = co_await bridge.invoke(*instance, "openDoor", 2.0);   // fn(self, 2.0)
//   ScriptValue plain = co_await bridge.callValue(fn);                         // plain-data variant
//
// sol objects must only be touched on the main thread: await these futures from the CoroutineScheduler thread.

#include <oxwald/async/future.hpp>
#include <oxwald/script/script_instance.hpp>
#include <oxwald/script/script_vm.hpp>

#include <glm/gtc/quaternion.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

namespace ox::script {

// Type-erased C++ future as seen by Lua (userdata "Future").
struct LuaFuture {
    std::shared_ptr<ox::detail::FutureStateBase> state;
    std::function<sol::object(sol::state_view)> extract; // value -> Lua; main thread only, state must be ready
};

class AsyncBridge {
public:
    // Registers the Future usertype and the global `await` in every (present and future) sandbox of `vm`.
    explicit AsyncBridge(ScriptVM& vm);
    ~AsyncBridge();
    AsyncBridge(const AsyncBridge&) = delete;
    AsyncBridge& operator=(const AsyncBridge&) = delete;

    // Main thread, once per frame before vm.update(): wakes script coroutines whose futures completed.
    void update();
    [[nodiscard]] usize pendingWakeups() const;

    // C++ future -> Lua Future userdata. T: void, bool, arithmetic, std::string, glm vec2/3/4, glm::quat,
    // ScriptValue, sol::object / sol::main_object (tables...), or anything sol2 can push.
    template <class T>
    sol::object wrap(const ox::Future<T>& future) {
        LuaFuture lf;
        lf.state = future.state();
        if constexpr (std::is_void_v<T>) {
            lf.extract = [](sol::state_view) { return sol::object(sol::lua_nil); };
        } else {
            std::shared_ptr<ox::detail::FutureState<T>> st = future.state();
            lf.extract = [this, st](sol::state_view L) { return toLua(L, st->get()); };
        }
        return sol::make_object(m_vm.lua(), std::move(lf));
    }

    // Runs fn(args...) as a script coroutine (owner 0) and completes with its first return value; a Lua error
    // fails the future with ox::AsyncError (message + location).
    template <class... Args>
    ox::Future<sol::main_object> call(const sol::protected_function& fn, Args&&... args) {
        return spawnCall(m_env, fn, std::forward<Args>(args)...);
    }
    // Calls the instance's global `function(self, args...)` as a coroutine owned by the instance (destroying the
    // instance cancels it: the future then fails with ox::BrokenPromise at the next Lua GC cycle).
    template <class... Args>
    ox::Future<sol::main_object> invoke(ScriptInstance& instance, std::string_view function, Args&&... args) {
        sol::object fn = instance.environment()[std::string(function)];
        if (fn.get_type() != sol::type::function) {
            return ox::makeErrorFuture<sol::main_object>(std::string("script has no function '") +
                                                         std::string(function) + "'");
        }
        return spawnCall(instance.environment(), fn.as<sol::protected_function>(), instance.self(),
                         std::forward<Args>(args)...);
    }
    // Plain-data variant (safe to read on any thread): tables become monostate.
    template <class... Args>
    ox::Future<ScriptValue> callValue(const sol::protected_function& fn, Args&&... args) {
        auto promise = std::make_shared<ox::Promise<ScriptValue>>();
        ox::Future<ScriptValue> out = promise->future();
        ox::Future<sol::main_object> inner = call(fn, std::forward<Args>(args)...);
        inner.onReady([promise, inner] {
            if (inner.hasError()) {
                promise->setException(inner.state()->exception());
            } else {
                promise->setValue(ScriptVM::fromLua(inner.get()));
            }
        });
        return out;
    }

    template <class T>
    sol::object toLua(sol::state_view L, const T& v) {
        if constexpr (std::is_same_v<T, ScriptValue>) {
            return m_vm.toLua(v);
        } else {
            return sol::make_object(L, v);
        }
    }

    ScriptVM& vm() { return m_vm; }

private:
    struct WakeQueue {
        std::mutex mutex;
        std::vector<u64> ids;
    };

    template <class... Args>
    ox::Future<sol::main_object> spawnCall(sol::environment& env, const sol::protected_function& fn, Args&&... args) {
        auto promise = std::make_shared<ox::Promise<sol::main_object>>();
        ox::Future<sol::main_object> future = promise->future();
        sol::object spawn = env["spawn"];
        if (spawn.get_type() != sol::type::function) {
            promise->setError("environment has no spawn()");
            return future;
        }
        auto [resolve, reject] = makeResolvers(promise);
        ScriptResult r = m_vm.call(spawn.as<sol::protected_function>(), "async call", m_callWrapper, fn, resolve,
                                   reject, std::forward<Args>(args)...);
        if (!r.ok) {
            promise->setError(r.error);
        }
        return future;
    }
    std::pair<sol::object, sol::object> makeResolvers(const std::shared_ptr<ox::Promise<sol::main_object>>& promise);
    bool park(sol::this_state ts, LuaFuture& f);

    ScriptVM& m_vm;
    std::shared_ptr<WakeQueue> m_wakes;
    sol::environment m_env;
    sol::protected_function m_callWrapper;
};

} // namespace ox::script
