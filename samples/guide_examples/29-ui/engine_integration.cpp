// Глава 29: подключение UI к движку — UiModule + withUi(render::createRenderer()) на headless-движке:
// F1 через InputSystem, консольные команды ui.*, загрузка документа, бенчмарк "Auto" (docs/guide/29-ui.md).
// Нужно Vulkan-устройство; без него тест пропускается.
#include <oxwald/core/scalability.hpp>
#include <oxwald/core/uuid.hpp>
#include <oxwald/render/runtime_renderer.hpp>
#include <oxwald/runtime/console.hpp>
#include <oxwald/runtime/engine.hpp>
#include <oxwald/runtime/settings.hpp>
#include <oxwald/ui/ui.hpp>

#include <gtest/gtest.h>

#include <filesystem>

using namespace ox;
namespace fs = std::filesystem;

TEST(GuideUiEngine, ModuleAndRendererDecorator) {
    const fs::path dir = fs::temp_directory_path() / ("oxwald_guide_ui_engine_" + Uuid::generate().toString());

    Engine engine;
    ui::UiConfig cfg;
    cfg.imguiConfig.visible = false; // оверлей скрыт до F1
    cfg.gameUIConfig.root = (ui::resourceDir() / "sample").string() + "/"; // в игре — project://UI/ (по умолчанию)
    engine.addModule(std::make_unique<ui::UiModule>(cfg));
    engine.setRenderer(ui::withUi(render::createRenderer({.headlessWidth = 640, .headlessHeight = 360})));

    EngineConfig ec;
    ec.appName = "GuideUi";
    ec.headless = true; // без окна: рендер в offscreen-цель
    ec.workerThreads = 2;
    ec.userDir = dir / "user";
    ec.loadUserSettings = false;
    ec.saveUserSettingsOnShutdown = false;
    if (Status st = engine.init(ec); !st) {
        fs::remove_all(dir);
        GTEST_SKIP() << "движок не запустился (нет Vulkan?): " << st.error().message;
    }

    auto* ui = engine.services().tryGet<ui::UiSystem>(); // сервис модуля
    ASSERT_NE(ui, nullptr);
    for (int i = 0; i < 4; ++i) engine.tick(1.0 / 60.0);
    EXPECT_TRUE(ui->bridge().hasRenderInfo()) << "UI-фича отработала в рендерере и вернула статистику";

    // F1 приходит через InputSystem движка: UI поглощает его, игра не видит.
    engine.input().inject(InputEvent::key(Key::F1, true));
    engine.tick(1.0 / 60.0);
    EXPECT_TRUE(ui->imgui()->visible());
    EXPECT_FALSE(engine.input().keyPressed(Key::F1));
    engine.input().inject(InputEvent::key(Key::F1, false));

    // Консольные команды модуля.
    ASSERT_TRUE(engine.console().execute("ui.debug").hasValue()); // переключить оверлей
    EXPECT_FALSE(ui->imgui()->visible());
    ASSERT_TRUE(engine.console().execute("ui.open Stats").hasValue()); // открыть окно и показать оверлей
    EXPECT_TRUE(ui->imgui()->visible());
    ASSERT_TRUE(engine.console().execute("ui.load main_menu.rml").hasValue()); // загрузить и показать документ
    EXPECT_TRUE(ui->gameUI()->isVisible("main_menu.rml"));
    ASSERT_TRUE(engine.console().execute("ui.hide main_menu.rml").hasValue());
    EXPECT_FALSE(ui->gameUI()->isVisible("main_menu.rml"));
    engine.tick(1.0 / 60.0);

    // "Auto" из меню настроек: бенчмарк на потоке рендера между кадрами, результат — на игровом потоке.
    std::string summary;
    ScopedConnection c = ui->qualityAutoDetected.connect([&](const std::string& s) { summary = s; });
    ui->bridge().autoDetectRequested.store(true); // это делает событие set_quality('Auto') модели settings
    for (int i = 0; i < 10 && summary.empty(); ++i) engine.tick(1.0 / 60.0);
    EXPECT_TRUE(summary.starts_with("Auto: ")) << summary; // "Auto: <уровень> (score <очки бенчмарка>)"

    engine.shutdown();
    scalability::setOverall(QualityLevel::High);
    std::error_code ec2;
    fs::remove_all(dir, ec2);
}
