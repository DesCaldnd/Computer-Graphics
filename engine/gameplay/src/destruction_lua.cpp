// Lua `destruction` table (see docs/guide/34-destruction.md):
//   destruction.applyDamage(entity, amount[, point[, impulse]]) -> broke (bool)
//   destruction.fracture(entity[, point[, impulse]])            -> bool
//   destruction.explode(center, radius, damage[, impulse])      -> number of affected destructibles / chunks
//   destruction.isBroken(entity), destruction.health(entity), destruction.chunks(entity), destruction.activeDebris()
#include "lua_types.hpp"

#include <oxwald/gameplay/destruction.hpp>
#include <oxwald/gameplay/script.hpp>

namespace ox::gameplay {

void bindDestructionLua(ScriptRuntime& scripts, DestructionRuntime& destruction) {
    script::ScriptVM& vm = scripts.vm();
    if (vm.hasApi("destruction")) return;
    ScriptRuntime* R = &scripts;
    DestructionRuntime* D = &destruction;
    vm.bindApi("destruction", [R, D](sol::state_view, sol::table& api) {
        api["applyDamage"] = [D](const lua::LuaEntity& e, f32 amount, sol::optional<glm::vec3> point,
                                 sol::optional<glm::vec3> impulse) {
            const Entity en = e.entity();
            if (!en.valid()) return false;
            return D->applyDamage(en, amount, point.value_or(en.worldPosition()), impulse.value_or(glm::vec3(0.f)),
                                  DestructionCause::Script);
        };
        api["fracture"] = [D](const lua::LuaEntity& e, sol::optional<glm::vec3> point, sol::optional<glm::vec3> impulse) {
            const Entity en = e.entity();
            if (!en.valid()) return false;
            return D->fracture(en, point.value_or(en.worldPosition()), impulse.value_or(glm::vec3(0.f)),
                               DestructionCause::Script);
        };
        api["explode"] = [D](const glm::vec3& center, f32 radius, f32 damage, sol::optional<f32> impulse) {
            return D->applyRadialDamage(center, radius, damage, impulse.value_or(0.f));
        };
        api["isBroken"] = [D](const lua::LuaEntity& e) { return D->isBroken(e.entity()); };
        api["health"] = [](const lua::LuaEntity& e) -> sol::optional<f32> {
            const Entity en = e.entity();
            const auto* d = en.valid() ? en.tryGet<DestructibleComponent>() : nullptr;
            if (!d) return sol::nullopt;
            return d->health;
        };
        api["chunks"] = [R, D](const lua::LuaEntity& e, sol::this_state ts) {
            sol::state_view lv(ts);
            sol::table out = lv.create_table();
            int i = 1;
            for (const Entity& c : D->chunksOf(e.entity())) out[i++] = lua::wrap(*R, c);
            return out;
        };
        api["activeDebris"] = [D]() { return D->activeDebrisCount(); };
        api["setDebugDraw"] = [D](bool on) { D->debugDraw = on; };
    });
}

} // namespace ox::gameplay
