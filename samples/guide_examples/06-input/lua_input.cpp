// Глава 06: Lua-таблица `input` (docs/guide/06-input.md). В Engine она подключается автоматически
// (встроенный модуль script); здесь — вручную, чтобы пример работал без движка.
#include <oxwald/runtime/input.hpp>
#include <oxwald/runtime/script_bindings.hpp>
#include <oxwald/script/script_vm.hpp>

#include <gtest/gtest.h>

#include <filesystem>

TEST(GuideInputLua, ReadActionsFromLua) {
    ox::InputSystem input;
    ox::InputMappingConfig m;
    m.actions = {{"Move", ox::InputValueType::Axis2D}, {"Jump", ox::InputValueType::Bool}};
    m.contexts = {{"OnFoot", 0, {
        {"Move", "Key.D"},
        {"Move", "Key.W", {ox::InputModifier::makeSwizzle()}},
        {"Jump", "Key.Space", {}, {ox::InputTrigger::pressed()}},
    }}};
    m.activeContexts = {"OnFoot"};
    input.setMappings(m);

    ox::script::ScriptVM vm;
    ox::bindInputLuaApi(vm, input); // Engine: делает встроенный модуль script

    auto env = vm.createEnvironment();
    auto loaded = vm.runFile(std::filesystem::path(OX_GUIDE_DIR) / "player_input.lua", &env);
    ASSERT_TRUE(loaded) << loaded.error;

    input.inject(ox::InputEvent::key(ox::Key::W, true));
    input.inject(ox::InputEvent::key(ox::Key::LeftShift, true));
    input.inject(ox::InputEvent::key(ox::Key::Space, true));
    input.update(1.0 / 60.0);

    auto r = vm.runString("local i = readPlayerInput(0.1); return i.jump and i.dx == 0 and i.dz == 1.0", "check", &env);
    ASSERT_TRUE(r) << r.error;
    EXPECT_TRUE(r.value.as<bool>());

    ASSERT_TRUE(vm.runString("lockCursorForGameplay()", "cursor", &env));
    EXPECT_EQ(input.cursorMode(), ox::CursorMode::Locked);
}
