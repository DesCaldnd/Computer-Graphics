// Глава 15: ScriptVM — песочница, выполнение кода, ошибки и лимиты, значения ScriptValue, bindApi
// (docs/guide/15-scripting-lua.md).
#include <oxwald/script/script_events.hpp>
#include <oxwald/script/script_vm.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

using namespace ox;
using namespace ox::script;

TEST(GuideLuaVm, RunCodeInSandbox) {
    ScriptVM vm({.memoryLimit = 64u << 20, .instructionLimit = 10'000'000});
    sol::environment env = vm.createEnvironment(); // у каждого скрипта своё окружение (глобалы не общие)

    ScriptResult r = vm.runString("return vec3(1, 2, 2):length()", "console", &env);
    ASSERT_TRUE(r) << r.error;
    EXPECT_FLOAT_EQ(r.value.as<f32>(), 3.f);

    // Опасного нет: io, os.execute, load/dofile, debug, package...
    r = vm.runString("return io == nil and os.execute == nil and load == nil and debug == nil", "console", &env);
    EXPECT_TRUE(r.value.as<bool>());
    // Глобалы одного окружения не видны в другом.
    vm.runString("score = 10", "a", &env);
    sol::environment other = vm.createEnvironment();
    EXPECT_TRUE(vm.runString("return score == nil", "b", &other).value.as<bool>());
}

TEST(GuideLuaVm, ErrorsAreLoggedNotThrown) {
    ScriptVM vm;
    sol::environment env = vm.createEnvironment();
    // Ошибка не бросает исключение в C++: она залогирована с chunk:line и traceback, VM продолжает работать.
    ScriptResult r = vm.runString("local t = nil\nreturn t.field", "scripts/broken.lua", &env);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("scripts/broken.lua:2:"), std::string::npos);
    EXPECT_EQ(vm.errorCount(), 1u);
    EXPECT_TRUE(vm.runString("return 1", "after", &env).ok);
}

TEST(GuideLuaVm, InstructionLimitStopsInfiniteLoops) {
    ScriptVM vm({.instructionLimit = 200'000}); // бюджет на один вызов из C++
    sol::environment env = vm.createEnvironment();
    ScriptResult r = vm.runString("while true do end", "loop.lua", &env);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("instruction limit"), std::string::npos);
    // pcall не «проглотит» превышение лимита.
    EXPECT_FALSE(vm.runString("pcall(function() while true do end end)", "p", &env).ok);
}

TEST(GuideLuaVm, ScriptValueConversions) {
    ScriptVM vm;
    // ScriptValue — простой C++ variant для свойств и событий: monostate, bool, i64, f64, string, vec2/3/4.
    sol::object v = vm.toLua(ScriptValue{glm::vec3(1.f, 2.f, 3.f)}); // -> Lua vec3
    sol::environment env = vm.createEnvironment();
    env["spawnPoint"] = v;
    ScriptResult r = vm.runString("return spawnPoint + vec3.up", "conv", &env);
    ScriptValue back = ScriptVM::fromLua(r.value);
    ASSERT_TRUE(std::holds_alternative<glm::vec3>(back));
    EXPECT_EQ(std::get<glm::vec3>(back), glm::vec3(1.f, 3.f, 3.f));
    EXPECT_TRUE(std::holds_alternative<i64>(ScriptVM::fromLua(vm.runString("return 7", "i", &env).value)));
    EXPECT_TRUE(std::holds_alternative<f64>(ScriptVM::fromLua(vm.runString("return 7.5", "f", &env).value)));
    EXPECT_EQ(toDebugString(ScriptValue{std::string("hi")}), "\"hi\"");
}

TEST(GuideLuaVm, BindApiExposesEngineFunctions) {
    ScriptVM vm;
    // Свой модуль движка в Lua: таблица только для чтения `physics` во всех песочницах.
    // Важно: привязывайте API до createEnvironment() — «сырые» окружения, созданные раньше, его не увидят.
    vm.bindApi("physics", [](sol::state_view, sol::table& api) {
        api["raycast"] = [](glm::vec3 from, glm::vec3 dir, f32 maxDist) -> sol::optional<glm::vec3> {
            if (dir.y >= 0.f) return sol::nullopt;      // «земля» — плоскость y = 0
            const f32 t = from.y / -dir.y;
            if (t > maxDist) return sol::nullopt;
            return from + dir * t;
        };
        api["gravity"] = -9.81;
    });
    sol::environment env = vm.createEnvironment();
    ScriptResult r = vm.runString(R"(
        local hit = physics.raycast(vec3(3, 10, 0), vec3(0, -1, 0), 100)
        local miss = physics.raycast(vec3(0, 1, 0), vec3(0, 1, 0), 100)
        return hit.x == 3 and hit.y == 0 and miss == nil and physics.gravity < 0
    )", "api", &env);
    ASSERT_TRUE(r) << r.error;
    EXPECT_TRUE(r.value.as<bool>());
    EXPECT_FALSE(vm.runString("physics.gravity = 0", "ro", &env).ok); // API-таблицы только для чтения
}

TEST(GuideLuaVm, EventBusBetweenCppAndLua) {
    ScriptVM vm;
    f64 damageTaken = 0.0;
    vm.events().subscribe("player.hurt", [&](std::string_view, const sol::object& payload) {
        damageTaken += payload.as<sol::table>()["amount"].get<f64>();
    });
    sol::environment env = vm.createEnvironment();
    ScriptResult r = vm.runString(R"(
        events.subscribe("lava.touch", function(name, dps)
            events.publish("player.hurt", { amount = dps * 0.5 })
        end)
    )", "lava", &env);
    ASSERT_TRUE(r) << r.error;
    vm.events().publish("lava.touch", ScriptValue{40.0}); // C++ -> Lua -> C++
    EXPECT_DOUBLE_EQ(damageTaken, 20.0);
}
