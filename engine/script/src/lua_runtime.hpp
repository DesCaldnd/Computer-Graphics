#pragma once

#include <oxwald/core/types.hpp>

#include <lua.hpp>

namespace ox::script {

class ScriptVM;

// Per-VM state reachable from any lua_State of the VM via lua_getallocf's user pointer. Used by the allocator
// (memory limit), the count hook (instruction limit) and raw C functions that need the VM.
struct LuaRuntime {
    ScriptVM* vm = nullptr;
    usize used = 0;
    usize peak = 0;
    usize limit = 0;
    u64 instructions = 0;
    u64 instructionLimit = 0;
    u32 hookInterval = 1000;
    bool exceeded = false; // sticky until the next top-level call: also aborts through pcall/resume in Lua
    int depth = 0;         // nesting of C++ -> Lua calls

    static LuaRuntime& of(lua_State* L) {
        void* ud = nullptr;
        lua_getallocf(L, &ud);
        return *static_cast<LuaRuntime*>(ud);
    }

    void beginTopLevelCall() {
        instructions = 0;
        exceeded = false;
    }
};

void* luaAllocate(void* ud, void* ptr, size_t osize, size_t nsize);
void luaCountHook(lua_State* L, lua_Debug* ar);
int luaTraceback(lua_State* L);
int luaCheckExceeded(lua_State* L);

} // namespace ox::script
