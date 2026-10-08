#include "lua_runtime.hpp"

#include <oxwald/script/script_events.hpp>

#include <algorithm>

namespace ox::script {

// ---------------------------------------------------------------- ScriptEventBus

ScriptEventBus::SubscriptionId ScriptEventBus::subscribe(std::string name, Handler handler, u64 owner) {
    const SubscriptionId id = ++m_nextId;
    m_byName[std::move(name)].push_back(std::make_shared<Entry>(Entry{id, owner, std::move(handler)}));
    return id;
}

void ScriptEventBus::unsubscribe(SubscriptionId id) {
    for (auto& [name, list] : m_byName) {
        for (auto& e : list) {
            if (e->id == id) {
                e->alive = false;
            }
        }
        std::erase_if(list, [](const std::shared_ptr<Entry>& e) { return !e->alive; });
    }
}

void ScriptEventBus::unsubscribeOwner(u64 owner) {
    if (owner == 0) {
        return;
    }
    for (auto& [name, list] : m_byName) {
        for (auto& e : list) {
            if (e->owner == owner) {
                e->alive = false;
            }
        }
        std::erase_if(list, [](const std::shared_ptr<Entry>& e) { return !e->alive; });
    }
}

void ScriptEventBus::publish(std::string_view name, const sol::object& payload) {
    auto it = m_byName.find(std::string(name));
    if (it == m_byName.end()) {
        return;
    }
    const auto snapshot = it->second; // handlers may (un)subscribe
    for (const auto& e : snapshot) {
        if (!e->alive) {
            continue;
        }
        try {
            e->handler(name, payload);
        } catch (const std::exception& ex) {
            m_vm.reportError(std::format("event '{}' handler", name), ex.what());
        }
    }
}

void ScriptEventBus::publish(std::string_view name, const ScriptValue& payload) { publish(name, m_vm.toLua(payload)); }

usize ScriptEventBus::subscriberCount(std::string_view name) const {
    auto it = m_byName.find(std::string(name));
    return it == m_byName.end() ? 0 : it->second.size();
}

// ---------------------------------------------------------------- Scheduler

Scheduler::~Scheduler() { clear(); }

void Scheduler::clear() {
    lua_State* L = m_vm.lua().lua_state();
    for (auto& [id, task] : m_tasks) {
        if (task.ref != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, task.ref);
        }
    }
    m_tasks.clear();
    m_timers.clear();
}

usize Scheduler::taskCount() const {
    return static_cast<usize>(
        std::count_if(m_tasks.begin(), m_tasks.end(), [](const auto& kv) { return !kv.second.cancelled; }));
}

bool Scheduler::inTask(lua_State* L) const { return !m_running.empty() && m_running.back() == L; }

void Scheduler::release(Task& task) {
    if (task.ref != LUA_NOREF) {
        luaL_unref(m_vm.lua().lua_state(), LUA_REGISTRYINDEX, task.ref);
        task.ref = LUA_NOREF;
    }
}

int Scheduler::spawnFromLua(lua_State* L, u64 owner) {
    const int n = lua_gettop(L); // function + arguments
    lua_State* co = lua_newthread(L);
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX); // pops the thread
    lua_xmove(L, co, n);
    const u64 id = ++m_nextId;
    Task& task = m_tasks[id];
    task.thread = co;
    task.ref = ref;
    task.owner = owner;
    task.wakeTime = m_time;
    resume(id, L, n - 1);
    lua_pushinteger(L, static_cast<lua_Integer>(id));
    return 1;
}

bool Scheduler::resume(u64 id, lua_State* from, int nargs) {
    auto it = m_tasks.find(id);
    if (it == m_tasks.end()) {
        return false;
    }
    lua_State* co = it->second.thread;
    it->second.running = true;
    m_running.push_back(co);
    m_runningIds.push_back(id);
    int nres = 0;
    int status;
    {
        ScriptVM::CallScope scope(m_vm);
        status = lua_resume(co, from, nargs, &nres);
    }
    m_running.pop_back();
    m_runningIds.pop_back();
    Task& task = m_tasks[id]; // std::map: nested spawns never invalidate it
    task.running = false;

    if (status == LUA_YIELD) {
        f64 delay = 0.0;
        if (nres > 0 && lua_type(co, -nres) == LUA_TNUMBER) {
            delay = lua_tonumber(co, -nres);
        }
        lua_pop(co, nres);
        task.wakeTime = m_time + std::max(0.0, delay);
        if (!task.cancelled) {
            return true;
        }
    } else if (status != LUA_OK) {
        const char* msg = lua_tostring(co, -1);
        luaL_traceback(from, co, msg ? msg : "(non-string error)", 0);
        std::string text = lua_tostring(from, -1);
        lua_pop(from, 1);
        m_vm.reportError("coroutine", text);
    }
    release(task);
    m_tasks.erase(id);
    return false;
}

u64 Scheduler::addTimer(f64 delay, f64 interval, sol::protected_function fn, u64 owner) {
    const u64 id = ++m_nextId;
    m_timers.push_back(Timer{id, owner, m_time + std::max(0.0, delay), interval, std::move(fn)});
    return id;
}

void Scheduler::wake(u64 id) {
    if (auto it = m_tasks.find(id); it != m_tasks.end() && !it->second.cancelled) {
        it->second.wakeTime = std::min(it->second.wakeTime, m_time);
    }
}

void Scheduler::cancel(u64 id) {
    for (Timer& t : m_timers) {
        if (t.id == id) {
            t.cancelled = true;
        }
    }
    if (auto it = m_tasks.find(id); it != m_tasks.end()) {
        it->second.cancelled = true;
        if (!it->second.running) {
            release(it->second);
            m_tasks.erase(it);
        }
    }
}

void Scheduler::cancelOwner(u64 owner) {
    for (Timer& t : m_timers) {
        if (t.owner == owner) {
            t.cancelled = true;
        }
    }
    for (auto it = m_tasks.begin(); it != m_tasks.end();) {
        if (it->second.owner == owner) {
            it->second.cancelled = true;
            if (!it->second.running) {
                release(it->second);
                it = m_tasks.erase(it);
                continue;
            }
        }
        ++it;
    }
}

void Scheduler::update(f64 dt) {
    m_time += dt;

    for (usize i = 0; i < m_timers.size(); ++i) {
        if (m_timers[i].cancelled || m_timers[i].next > m_time) {
            continue;
        }
        sol::protected_function fn = m_timers[i].fn; // callbacks may add timers (vector reallocation)
        if (m_timers[i].interval > 0.0) {
            m_timers[i].next += m_timers[i].interval;
            if (m_timers[i].next <= m_time) {
                m_timers[i].next = m_time + m_timers[i].interval;
            }
        } else {
            m_timers[i].cancelled = true;
        }
        m_vm.call(fn, "timer");
    }
    std::erase_if(m_timers, [](const Timer& t) { return t.cancelled; });

    std::vector<u64> due;
    for (auto& [id, task] : m_tasks) {
        if (!task.cancelled && !task.running && task.wakeTime <= m_time) {
            due.push_back(id);
        }
    }
    lua_State* L = m_vm.lua().lua_state();
    for (u64 id : due) {
        auto it = m_tasks.find(id);
        if (it != m_tasks.end() && !it->second.cancelled) {
            resume(id, L, 0);
        }
    }
}

} // namespace ox::script
