#pragma once

#include <oxwald/gameplay/script.hpp>
#include <oxwald/scene/component_registry.hpp>

#include <string>

// Lua-side handle types of the gameplay entity API (registered as sol2 usertypes in lua_bindings.cpp).
namespace ox::gameplay::lua {

// Entity handle. Becomes invalid when the entity is destroyed or the runtime switches worlds.
struct LuaEntity {
    ScriptRuntime* rt = nullptr;
    World* world = nullptr;
    entt::entity handle = entt::null;

    // Entities scheduled for destruction stay valid until the end of the frame (onDestroy can still use them).
    [[nodiscard]] bool valid() const { return rt && world && rt->world() == world && world->valid(handle); }
    [[nodiscard]] Entity entity() const { return valid() ? world->wrap(handle) : Entity{}; }
    friend bool operator==(const LuaEntity& a, const LuaEntity& b) { return a.world == b.world && a.handle == b.handle; }
};

[[nodiscard]] inline LuaEntity wrap(ScriptRuntime& rt, Entity e) {
    return e.valid() ? LuaEntity{&rt, e.world(), e.handle()} : LuaEntity{};
}

// Reflection-driven proxy of a component (or of a nested struct/array/map inside it, addressed by `path`).
struct LuaComponent {
    LuaEntity owner;
    const ComponentInfo* info = nullptr;
    std::string path;
};

struct LuaTransform {
    LuaEntity owner;
};
struct LuaBody {
    LuaEntity owner;
};
struct LuaAgent {
    LuaEntity owner;
};
struct LuaAnimator {
    LuaEntity owner;
};
struct LuaSpline {
    LuaEntity owner;
};

// Registers the usertypes and API tables on the runtime's VM (idempotent per VM).
void installBindings(ScriptRuntime& rt);

} // namespace ox::gameplay::lua
