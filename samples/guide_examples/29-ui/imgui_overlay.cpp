// Глава 29: отладочный оверлей Dear ImGui — кадр ImGui, клавиши F1/~, свои окна в меню оверлея, HUD поверх игры,
// встроенные окна DebugTools и консоль (docs/guide/29-ui.md). Окно и Vulkan не нужны: UiFrame строится на CPU.
#include <oxwald/core/cvar.hpp>
#include <oxwald/runtime/console.hpp>
#include <oxwald/runtime/input.hpp>
#include <oxwald/runtime/settings.hpp>
#include <oxwald/ui/ui.hpp>

#include <imgui.h>

#include <gtest/gtest.h>

using namespace ox;
using namespace ox::ui;

namespace {

// Только ImGui: игровой UI (RmlUi) в этих примерах не нужен.
UiConfig imguiOnly() {
    UiConfig c;
    c.gameUI = false;
    return c;
}

void frame(UiSystem& ui) {
    ui.beginFrame(1.0 / 60.0, {1280, 720}, 1.0f);
    ui.endFrame();
}

} // namespace

// Слой ImGui сам по себе: свой контекст, шрифты в UiTextureStore, результат — UiFrame для рендера.
TEST(GuideUiImGui, FrameProducesDrawCommands) {
    UiRenderBridge bridge;
    ImGuiLayer imgui(bridge.textures());
    UiFrame out;
    for (int i = 0; i < 2; ++i) { // новое окно с автоматическим размером невидимо в свой первый кадр
        out = {};
        imgui.beginFrame(1.0 / 60.0, {1920, 1080}, /*dpiScale*/ 2.0f); // ImGui работает в точках: 960×540
        ImGui::Begin("Привет");
        ImGui::Text("Кириллица работает без диапазонов глифов");
        ImGui::End();
        imgui.endFrame(out);
    }
    EXPECT_EQ(out.size, glm::uvec2(1920, 1080)); // UiFrame — в пикселях кадра
    EXPECT_FALSE(out.commands.empty());
    EXPECT_GE(bridge.textures().size(), 1u) << "атлас шрифта попал в UiTextureStore";
}

// F1 и ~ переключают оверлей; нажатие поглощается и не доходит до игры.
TEST(GuideUiImGui, ToggleKeys) {
    UiRenderBridge bridge;
    ImGuiLayer imgui(bridge.textures(), {.visible = false, .toggleKeys = {Key::F1, Key::GraveAccent}});
    EXPECT_FALSE(imgui.visible());
    EXPECT_TRUE(imgui.processEvent(InputEvent::key(Key::F1, true)));   // true = поглощено UI
    EXPECT_TRUE(imgui.visible());
    EXPECT_FALSE(imgui.processEvent(InputEvent::key(Key::F1, false))); // отпускания не поглощаются никогда
    imgui.toggle();
    EXPECT_FALSE(imgui.visible());
}

// Своё отладочное окно: пункт в меню «Windows» оверлея, рисуется, пока оверлей открыт и окно не закрыто.
TEST(GuideUiImGui, CustomDebugWindow) {
    UiSystem ui(imguiOnly());
    ImGuiLayer& imgui = *ui.imgui();

    int enemies = 3;
    int drawn = 0;
    const u32 id = imgui.addWindow("Game/Spawner", [&](bool& open) {
        ++drawn;
        if (ImGui::Begin("Spawner", &open)) { // крестик окна сбрасывает open → пункт меню снимается
            ImGui::Text("Врагов: %d", enemies);
            if (ImGui::Button("Spawn")) ++enemies;
            ImGui::SliderInt("Count", &enemies, 0, 100);
        }
        ImGui::End();
    });

    // HUD, который виден всегда — даже при закрытом оверлее (как ui.ShowStats).
    int hudFrames = 0;
    imgui.addAlwaysOnTop([&] {
        ++hudFrames;
        ImGui::GetForegroundDrawList()->AddText(ImVec2(8, 8), IM_COL32(255, 255, 255, 255), "FPS 60");
    });
    // Свои пункты в главном меню оверлея.
    imgui.addMenu([] {
        if (ImGui::BeginMenu("Game")) {
            ImGui::MenuItem("Kill all enemies");
            ImGui::EndMenu();
        }
    });

    frame(ui); // оверлей скрыт и окно закрыто: окно не рисуется, HUD — рисуется
    EXPECT_EQ(drawn, 0);
    EXPECT_EQ(hudFrames, 1);

    imgui.setVisible(true);                    // то же, что F1 или консольная команда ui.debug
    imgui.setWindowOpen("Game/Spawner", true); // то же, что выбрать пункт в меню Windows
    frame(ui);
    frame(ui);
    EXPECT_EQ(drawn, 2);
    EXPECT_TRUE(imgui.windowOpen("Game/Spawner"));
    EXPECT_GT(ui.lastFrame()->commands.size(), 3u);

    imgui.removeWindow(id); // например, при выгрузке игрового модуля
    frame(ui);
    EXPECT_EQ(drawn, 2);
}

// Окна, которые ImGui-код открывает напрямую (ImGui::Begin в игровом коде), видны и без оверлея.
TEST(GuideUiImGui, DirectImGuiCallsDuringTheFrame) {
    UiSystem ui(imguiOnly());
    for (int i = 0; i < 2; ++i) {
        ui.beginFrame(1.0 / 60.0, {1280, 720}); // в движке это делает UiModule::preUpdate
        ASSERT_TRUE(ui.inFrame());
        ImGui::Begin("Player");                 // любой игровой код между beginFrame и endFrame
        ImGui::Text("HP 100");
        ImGui::End();
        ui.endFrame();                          // в движке — по сигналу Engine::frameEnded
    }
    EXPECT_FALSE(ui.imgui()->visible());
    EXPECT_FALSE(ui.lastFrame()->empty());
    EXPECT_EQ(ui.bridge().latest(), ui.lastFrame()) << "кадр опубликован для потока рендера";
}

static CVar<int> cvGuideEnemies("game.Guide.MaxEnemies", 8, "пример cvar для окна консоли (глава 29)");

// Встроенные окна DebugTools без движка: консоль, настройки и cvar'ы, статистика.
TEST(GuideUiImGui, DebugToolsAndConsole) {
    UiSystem ui(imguiOnly());
    Console console;
    Settings settings;
    // С движком UiSystem::attach делает это сам: DebugToolsContext{.engine = &engine}.
    DebugTools& tools = ui.enableDebugTools({.console = &console, .settings = &settings});
    ui.imgui()->setVisible(true);
    tools.open(DebugTools::kConsole);
    tools.open(DebugTools::kSettings); // "Settings & CVars"
    tools.open(DebugTools::kStats);
    frame(ui);
    frame(ui);
    EXPECT_GT(ui.lastFrame()->vertices.size(), 1000u);

    // Строка, введённая в окне Console, идёт в runtime Console — как набранная команда.
    tools.consoleSubmit("game.Guide.MaxEnemies 32");
    EXPECT_EQ(cvGuideEnemies.get(), 32);
    EXPECT_EQ(console.history().back(), "game.Guide.MaxEnemies 32");

    // HUD статистики виден и при скрытом оверлее.
    ASSERT_TRUE(CVarRegistry::instance().set("ui.ShowStats", "true", CVarSource::Code));
    ui.imgui()->setVisible(false);
    frame(ui);
    frame(ui);
    EXPECT_FALSE(ui.lastFrame()->empty());
    CVarRegistry::instance().set("ui.ShowStats", "false", CVarSource::Code);
    cvGuideEnemies.reset();
}
