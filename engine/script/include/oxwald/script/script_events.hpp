#pragma once

#include <oxwald/script/script_vm.hpp>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ox::script {

// Named event bus shared by Lua (`events.subscribe/publish/unsubscribe`) and C++. Payloads are Lua values
// (tables, numbers, userdata...); C++ code can convert with ScriptVM::fromLua / toLua.
// Lua subscriptions are owned by their script instance and dropped when it is destroyed.
// Bridge to the engine event system: subscribe here and forward, or publish from engine callbacks.
class ScriptEventBus {
public:
    using Handler = std::function<void(std::string_view name, const sol::object& payload)>;
    using SubscriptionId = u64;

    explicit ScriptEventBus(ScriptVM& vm) : m_vm(vm) {}

    SubscriptionId subscribe(std::string name, Handler handler, u64 owner = 0);
    void unsubscribe(SubscriptionId id);
    void unsubscribeOwner(u64 owner);
    // Handlers run synchronously; (un)subscribing from a handler is safe. Lua errors are logged, not propagated.
    void publish(std::string_view name, const sol::object& payload);
    void publish(std::string_view name, const ScriptValue& payload = {});
    usize subscriberCount(std::string_view name) const;
    void clear() { m_byName.clear(); }

private:
    struct Entry {
        SubscriptionId id;
        u64 owner;
        Handler handler;
        bool alive = true;
    };
    ScriptVM& m_vm;
    std::unordered_map<std::string, std::vector<std::shared_ptr<Entry>>> m_byName;
    SubscriptionId m_nextId = 0;
};

// Coroutine/timer scheduler on script time (advanced by ScriptVM::update). Lua API per sandbox:
//   spawn(fn, ...)            start a coroutine now (runs until its first wait); returns task id
//   wait(seconds)             only inside spawn()ed coroutines
//   time()                    script time in seconds
//   timer.after(s, fn) / timer.every(s, fn) -> id ; timer.cancel(id)
class Scheduler {
public:
    explicit Scheduler(ScriptVM& vm) : m_vm(vm) {}
    ~Scheduler();

    f64 time() const { return m_time; }
    void update(f64 dt);
    void cancelOwner(u64 owner);
    usize taskCount() const;
    usize timerCount() const { return m_timers.size(); }

    // Internal entry points used by the Lua bindings.
    int spawnFromLua(struct lua_State* L, u64 owner);
    u64 addTimer(f64 delay, f64 interval, sol::protected_function fn, u64 owner);
    void cancel(u64 id);
    bool inTask(struct lua_State* L) const;
    void clear();

private:
    struct Task {
        struct lua_State* thread = nullptr;
        int ref = -2; // LUA_NOREF
        u64 owner = 0;
        f64 wakeTime = 0.0;
        bool running = false;
        bool cancelled = false;
    };
    struct Timer {
        u64 id;
        u64 owner;
        f64 next;
        f64 interval; // 0 = one-shot
        sol::protected_function fn;
        bool cancelled = false;
    };
    // Resumes with nargs already on the thread's stack. Returns false when the task finished or failed.
    bool resume(u64 id, struct lua_State* from, int nargs);
    void release(Task& task);

    ScriptVM& m_vm;
    f64 m_time = 0.0;
    u64 m_nextId = 0;
    std::map<u64, Task> m_tasks;
    std::vector<Timer> m_timers;
    std::vector<struct lua_State*> m_running; // stack of currently resumed task threads
};

} // namespace ox::script
