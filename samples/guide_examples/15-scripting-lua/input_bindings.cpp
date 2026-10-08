// Глава 15: привязки сервисов движка к Lua — таблица `input` из runtime (bindInputLuaApi)
// (docs/guide/15-scripting-lua.md). Скрипт: scripts/player_controller.lua.
#include <oxwald/runtime/input.hpp>
#include <oxwald/runtime/script_bindings.hpp>
#include <oxwald/script/script_instance.hpp>
#include <oxwald/script/script_vm.hpp>

#include <gtest/gtest.h>

#include <filesystem>

using namespace ox;

TEST(GuideLuaInput, ScriptReadsInputActions) {
    // В движке это делает модуль script (ScriptVM в Services + bindInputLuaApi); здесь — вручную, без окна.
    InputSystem input;
    input.addAction({"Move", InputValueType::Axis2D});
    input.addAction({"Jump", InputValueType::Bool});
    InputBinding moveRight;
    moveRight.action = "Move";
    moveRight.source = "Key.D";
    InputBinding jump;
    jump.action = "Jump";
    jump.source = "Key.Space";
    jump.triggers = {InputTrigger::pressed()};
    input.addContext({"Gameplay", 0, {moveRight, jump}});
    input.activateContext("Gameplay");

    script::ScriptVM vm;
    bindInputLuaApi(vm, input); // глобальная read-only таблица `input` во всех песочницах
    auto player = vm.createInstance(
        vm.loadScript(std::filesystem::path(OX_GUIDE_DIR) / "scripts" / "player_controller.lua"));
    ASSERT_TRUE(player->create());

    input.inject(InputEvent::key(Key::D, true));
    input.inject(InputEvent::key(Key::Space, true));
    input.inject(InputEvent::key(Key::LeftShift, true));
    input.update(0.5);      // сначала ввод...
    player->update(0.5f);   // ...потом скрипты

    const glm::vec3 pos = player->self()["position"].get<glm::vec3>();
    EXPECT_FLOAT_EQ(pos.x, 2.f); // speed 4 * 0.5 с
    EXPECT_EQ(player->self()["jumps"].get<int>(), 1);
    EXPECT_TRUE(player->self()["sprinting"].get<bool>());

    input.update(0.5); // Space всё ещё зажат, но триггер `pressed` срабатывает один раз
    player->update(0.5f);
    EXPECT_EQ(player->self()["jumps"].get<int>(), 1);
    EXPECT_EQ(vm.errorCount(), 0u) << vm.lastError();
}
