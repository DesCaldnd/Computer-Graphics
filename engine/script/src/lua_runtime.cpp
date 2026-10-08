#include "lua_runtime.hpp"

#include <cstdlib>

namespace ox::script {

// Must stay free of C++ objects with destructors: Lua errors raised from here longjmp.

void* luaAllocate(void* ud, void* ptr, size_t osize, size_t nsize) {
    auto* rt = static_cast<LuaRuntime*>(ud);
    const size_t old = ptr ? osize : 0; // for new blocks osize is the object type, not a size
    if (nsize == 0) {
        if (ptr) {
            rt->used -= old;
            std::free(ptr);
        }
        return nullptr;
    }
    if (rt->limit != 0 && nsize > old && rt->used + (nsize - old) > rt->limit) {
        return nullptr; // Lua raises "not enough memory" (shrinking never fails)
    }
    void* p = std::realloc(ptr, nsize);
    if (!p) {
        return nullptr;
    }
    rt->used = rt->used - old + nsize;
    if (rt->used > rt->peak) {
        rt->peak = rt->used;
    }
    return p;
}

void luaCountHook(lua_State* L, lua_Debug* ar) {
    if (ar->event != LUA_HOOKCOUNT) {
        return;
    }
    LuaRuntime& rt = LuaRuntime::of(L);
    rt.instructions += rt.hookInterval;
    if (rt.instructionLimit != 0 && (rt.exceeded || rt.instructions > rt.instructionLimit)) {
        rt.exceeded = true;
        lua_getinfo(L, "Sl", ar);
        luaL_error(L, "%s:%d: instruction limit exceeded (%I instructions) - infinite loop?", ar->short_src,
                   ar->currentline, static_cast<lua_Integer>(rt.instructionLimit));
    }
}

// Message handler: no Lua code runs here, so it works even after the instruction limit tripped.
int luaTraceback(lua_State* L) {
    const char* msg = lua_tostring(L, 1);
    if (!msg) {
        if (luaL_callmeta(L, 1, "__tostring") && lua_type(L, -1) == LUA_TSTRING) {
            return 1;
        }
        msg = lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
    }
    luaL_traceback(L, L, msg, 1);
    return 1;
}

// Wraps sandbox pcall/xpcall/coroutine.resume results: returns its arguments unchanged unless the instruction limit
// tripped, in which case the abort continues upward instead of being swallowed by the script.
int luaCheckExceeded(lua_State* L) {
    if (LuaRuntime::of(L).exceeded) {
        lua_pushliteral(L, "instruction limit exceeded - aborting script");
        return lua_error(L);
    }
    return lua_gettop(L);
}

} // namespace ox::script
