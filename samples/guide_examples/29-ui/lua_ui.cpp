// Глава 29: игровой и отладочный UI из Lua — таблицы `ui` (RmlUi: документы, модели, события) и `debug`
// (ImGui в немедленном режиме) (docs/guide/29-ui.md). Скрипт: scripts/quest_ui.lua, документ: ui/quest.rml.
#include <oxwald/script/script_instance.hpp>
#include <oxwald/script/script_vm.hpp>
#include <oxwald/ui/ui.hpp>

#include <RmlUi/Core.h>

#include <gtest/gtest.h>

#include <filesystem>

using namespace ox;
using namespace ox::ui;
namespace fs = std::filesystem;

namespace {
const fs::path kGuide = fs::path(OX_GUIDE_DIR);
} // namespace

#if OX_UI_HAS_SCRIPT

TEST(GuideUiLua, ScriptDrivesGameUiAndDebugWindow) {
    // Порядок важен: VM живёт дольше UiSystem (привязки держат Lua-функции). В движке это обеспечено:
    // сервис UiSystem уничтожается раньше ScriptVM.
    script::ScriptVM vm({.searchRoots = {kGuide / "scripts"}});
    UiConfig cfg;
    cfg.gameUIConfig.root = (kGuide / "ui").string() + "/";
    cfg.gameUIConfig.hotReload = false;
    UiSystem ui(cfg);
    ui.bindLua(vm); // в движке — UiSystem::attach, если есть сервис ScriptVM

    auto quest = vm.createInstance(vm.loadScript(kGuide / "scripts" / "quest_ui.lua"));
    ASSERT_TRUE(quest->create()); // onCreate: ui.createModel + ui.load + ui.on
    Rml::ElementDocument* doc = ui.gameUI()->document("quest.rml");
    ASSERT_NE(doc, nullptr);
    EXPECT_TRUE(ui.gameUI()->isVisible("quest.rml"));

    // Кадр: debug.* работает только между beginFrame и endFrame (вне кадра вызовы — no-op).
    ui.beginFrame(1.0 / 60.0, {1280, 720});
    quest->update(1.0f / 60.0f); // onUpdate: debug.window("Quest debug", ...)
    ui.endFrame();
    EXPECT_EQ(doc->GetElementById("title")->GetInnerRML(), "Волки у мельницы");
    EXPECT_EQ(doc->GetElementById("reward")->GetInnerRML(), "Награда: 50 золотых");

    // model:set помечает поле «грязным» — документ обновится на следующем кадре.
    ASSERT_TRUE(quest->invoke("setReward", 75).ok);
    ui.beginFrame(1.0 / 60.0, {1280, 720});
    ui.endFrame();
    EXPECT_EQ(doc->GetElementById("reward")->GetInnerRML(), "Награда: 75 золотых");

    // data-event-click="accept(reward)" → событие модели в Lua с аргументами args[1]...
    doc->GetElementById("accept")->Click();
    // ui.on("quest.rml", "close", "click", fn) → fn(ev) с полями type, target, value, x, y.
    doc->GetElementById("close")->Click();
    ui.beginFrame(1.0 / 60.0, {1280, 720});
    ui.endFrame();
    EXPECT_EQ(quest->self()["accepted"].get<int>(), 75);
    EXPECT_TRUE(quest->self()["closed"].get<bool>());
    EXPECT_FALSE(ui.gameUI()->isVisible("quest.rml"));
    EXPECT_EQ(vm.errorCount(), 0u) << vm.lastError();
    EXPECT_EQ(quest->errorCount(), 0u);

    quest.reset();
    ui.unbindLua(); // явно отвязать до уничтожения VM (здесь VM и так переживёт UiSystem)
}

// Мелкие функции `ui`/`debug` прямо из строки Lua (консоль, быстрые эксперименты).
TEST(GuideUiLua, ImmediateModeDebugApi) {
    script::ScriptVM vm;
    UiConfig cfg;
    cfg.gameUI = false; // только ImGui
    UiSystem ui(cfg);
    ui.bindLua(vm);
    sol::environment env = vm.createEnvironment();

    ASSERT_TRUE(vm.runString(R"(
        god, speed, name, tint = false, 5.0, "Оксвальд", vec3(1, 0.5, 0.25)
        function drawDebug()
            shown = debug.window("Player", function()
                debug.text("hp", 100)                               -- аргументы склеиваются через пробел
                if debug.button("Heal") then hp = 100 end
                god = debug.checkbox("God mode", god)               -- второе значение: changed
                speed = debug.sliderFloat("Speed", speed, 0, 20)
                name = debug.inputText("Name", name)
                tint = debug.colorEdit("Tint", tint)                -- vec3 / vec4 / {r, g, b, a}
                debug.separator()
                debug.plotLines("fps", {58, 60, 61, 59}, "fps", 0, 120, 60)
            end)
            debug.beginWindow("Manual")                             -- endWindow можно забыть: окно закроется в конце кадра
            debug.text("overlay visible:", tostring(debug.overlayVisible()))
        end
    )", "debug_ui", &env).ok);
    for (int i = 0; i < 2; ++i) { // новое окно ImGui с авторазмером невидимо в свой первый кадр
        ui.beginFrame(1.0 / 60.0, {1280, 720});
        ASSERT_TRUE(vm.runString("drawDebug()", "frame", &env).ok); // каждый кадр, как onUpdate
        ui.endFrame();
    }
    EXPECT_TRUE(env["shown"].get<bool>());
    EXPECT_EQ(env["name"].get<std::string>(), "Оксвальд");
    EXPECT_FALSE(ui.lastFrame()->empty());

    // Вне кадра — no-op, без ошибок.
    EXPECT_TRUE(vm.runString("return debug.button('x')", "outside", &env).ok);
    EXPECT_EQ(vm.errorCount(), 0u) << vm.lastError();
    ui.unbindLua();
}

#else
TEST(GuideUiLua, SkippedWithoutScriptModule) { GTEST_SKIP() << "ui собран без модуля script"; }
#endif
