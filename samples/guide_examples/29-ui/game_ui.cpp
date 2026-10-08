// Глава 29: игровой UI на RmlUi — документы, модели данных (data bindings), события, локализация, hot reload,
// маршрутизация ввода, меню настроек графики (модель `settings`) и образцы из engine/ui/resources/sample/
// (docs/guide/29-ui.md). Окно и Vulkan не нужны. Документы примера: ui/scoreboard.rml + ui/style.rcss.
#include <oxwald/core/cvar.hpp>
#include <oxwald/core/scalability.hpp>
#include <oxwald/core/uuid.hpp>
#include <oxwald/render/render_settings.hpp>
#include <oxwald/runtime/input.hpp>
#include <oxwald/runtime/settings.hpp>
#include <oxwald/ui/ui.hpp>

#include <RmlUi/Core.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace ox;
using namespace ox::ui;
namespace fs = std::filesystem;

namespace {

const fs::path kUiDir = fs::path(OX_GUIDE_DIR) / "ui";

// Только игровой UI; документы ищутся относительно root (в движке по умолчанию project://UI/).
UiConfig gameUiConfig(const fs::path& root, bool hotReload = false) {
    UiConfig c;
    c.imgui = false;
    c.gameUIConfig.root = root.string() + "/";
    c.gameUIConfig.hotReload = hotReload;
    c.gameUIConfig.hotReloadInterval = 0.0; // в игре по умолчанию 0.5 с; здесь — проверка каждый кадр
    return c;
}

void frame(UiSystem& ui, glm::uvec2 size = {1280, 720}) {
    ui.beginFrame(1.0 / 60.0, size, 1.0f); // RmlUi update + проверка hot reload
    ui.endFrame();                         // RmlUi render → UiFrame
}

// Временная копия ui/ для hot reload (не трогаем файлы в репозитории).
class TempUiDir {
public:
    TempUiDir() : m_path(fs::temp_directory_path() / ("oxwald_guide_ui_" + Uuid::generate().toString())) {
        fs::create_directories(m_path);
        for (const auto& e : fs::directory_iterator(kUiDir)) fs::copy_file(e.path(), m_path / e.path().filename());
    }
    ~TempUiDir() {
        std::error_code ec;
        fs::remove_all(m_path, ec);
    }
    [[nodiscard]] const fs::path& path() const { return m_path; }

private:
    fs::path m_path;
};

std::string readText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

} // namespace

// Модель данных C++ → документ: {{переменные}}, data-class-*, data-if, data-for, data-event-click; плюс
// обработчик по id элемента и локализация #ключей.
TEST(GuideUiGame, DataModelEventsAndTranslations) {
    UiSystem ui(gameUiConfig(kUiDir));
    GameUI& game = *ui.gameUI();

    int points = 120;
    int health = 80;
    Rml::String player = "Оксвальд";
    std::vector<Rml::String> enemies{"Волк", "Медведь"};
    int restarts = 0;
    {
        // Модель создаётся ДО загрузки документа с data-model="score".
        Rml::DataModelConstructor c = game.createModel("score");
        ASSERT_TRUE(bool(c));
        c.RegisterArray<std::vector<Rml::String>>(); // массивы и структуры регистрируются один раз на модель
        c.Bind("points", &points);
        c.Bind("health", &health);
        c.Bind("player", &player);
        c.Bind("enemies", &enemies);
        c.BindEventCallback("restart", [&](Rml::DataModelHandle model, Rml::Event&, const Rml::VariantList& args) {
            restarts += args.empty() ? 1 : args[0].Get<int>(); // data-event-click="restart(1)"
            points = 0;
            model.DirtyVariable("points");
        });
    }
    game.setTranslations({{"score_title", "Счёт"}}); // текст "#score_title" → "Счёт"

    Rml::ElementDocument* doc = game.load("scoreboard.rml", /*show*/ true);
    ASSERT_NE(doc, nullptr);
    int quits = 0;
    game.addEventListener("scoreboard.rml", "quit", "click", [&](Rml::Event& e) {
        ++quits;
        EXPECT_EQ(e.GetType(), "click");
    });
    frame(ui);

    EXPECT_EQ(doc->GetElementById("title")->GetInnerRML(), "Счёт");
    EXPECT_EQ(doc->GetElementById("points")->GetInnerRML(), "Очки: 120");
    EXPECT_FALSE(doc->GetElementById("health")->IsClassSet("danger"));
    Rml::ElementList items;
    doc->GetElementById("enemies")->GetElementsByTagName(items, "li");
    usize visibleItems = 0;
    for (Rml::Element* li : items) visibleItems += li->IsVisible() ? 1 : 0;
    EXPECT_EQ(visibleItems, 2u);

    // C++ поменял значение — сообщаем модели, какое поле «грязное»; документ обновится в следующем update.
    health = 10;
    enemies.push_back("Кабан");
    game.model("score").DirtyVariable("health");
    game.model("score").DirtyVariable("enemies");
    frame(ui);
    EXPECT_TRUE(doc->GetElementById("health")->IsClassSet("danger"));
    items.clear();
    doc->GetElementById("enemies")->GetElementsByTagName(items, "li");
    visibleItems = 0;
    for (Rml::Element* li : items) visibleItems += li->IsVisible() ? 1 : 0;
    EXPECT_EQ(visibleItems, 3u);

    doc->GetElementById("restart")->Click(); // как щелчок мышью
    doc->GetElementById("quit")->Click();
    frame(ui);
    EXPECT_EQ(restarts, 1);
    EXPECT_EQ(quits, 1);
    EXPECT_EQ(doc->GetElementById("points")->GetInnerRML(), "Очки: 0");

    game.hide("scoreboard.rml");
    EXPECT_FALSE(game.isVisible("scoreboard.rml"));
    EXPECT_EQ(game.errorCount(), 0u) << "ошибки RmlUi (парсинг, неизвестные переменные) пишутся в лог";
}

// Hot reload: правка .rcss/.rml рядом с загруженными документами применяется без перезапуска.
TEST(GuideUiGame, HotReloadOfStyleSheet) {
    TempUiDir dir;
    UiSystem ui(gameUiConfig(dir.path(), /*hotReload*/ true));
    GameUI& game = *ui.gameUI();
    int points = 0, health = 100;
    std::vector<Rml::String> enemies;
    {
        Rml::DataModelConstructor c = game.createModel("score");
        c.RegisterArray<std::vector<Rml::String>>();
        c.Bind("points", &points);
        c.Bind("health", &health);
        c.Bind("enemies", &enemies);
    }
    ASSERT_NE(game.load("scoreboard.rml", true), nullptr);
    frame(ui);
    auto panelColour = [&] { return game.document("scoreboard.rml")->GetElementById("panel")->GetComputedValues().background_color(); };
    EXPECT_EQ(panelColour(), Rml::Colourb(0x15, 0x1b, 0x26, 255));

    int reloaded = 0;
    ScopedConnection c = game.documentReloaded.connect([&](const std::string& name) {
        EXPECT_EQ(name, "scoreboard.rml");
        ++reloaded;
    });
    // «Художник» меняет цвет панели и сохраняет файл.
    std::string css = readText(dir.path() / "style.rcss");
    css.replace(css.find("#151b26"), 7, "#3d8bff");
    std::ofstream(dir.path() / "style.rcss", std::ios::binary) << css;
    fs::last_write_time(dir.path() / "style.rcss", fs::last_write_time(dir.path() / "style.rcss") + std::chrono::seconds(2));

    frame(ui); // update() опрашивает mtime и перезагружает документы, сохраняя их видимость
    EXPECT_EQ(reloaded, 1);
    EXPECT_TRUE(game.isVisible("scoreboard.rml"));
    EXPECT_EQ(panelColour(), Rml::Colourb(0x3d, 0x8b, 0xff, 255));

    EXPECT_EQ(game.pollHotReload(/*force*/ true), 1u); // то же делает консольная команда ui.reload
}

// Ввод: UI получает события первым; клики по «прозрачному» HUD (pointer-events: none) уходят в игру.
TEST(GuideUiGame, InputRoutingAndClickThrough) {
    UiConfig cfg = gameUiConfig(ui::resourceDir() / "sample");
    UiSystem ui(cfg);
    GameUI& game = *ui.gameUI();
    int health = 80, maxHealth = 100, ammo = 12, reserve = 60;
    Rml::String objective = "Найдите выход";
    {
        Rml::DataModelConstructor c = game.createModel("hud");
        c.Bind("health", &health);
        c.Bind("maxHealth", &maxHealth);
        c.Bind("ammo", &ammo);
        c.Bind("reserve", &reserve);
        c.Bind("objective", &objective);
    }
    ASSERT_NE(game.load("hud.rml", true), nullptr);
    int pauses = 0;
    game.addEventListener("hud.rml", "pause", "click", [&](Rml::Event&) { ++pauses; });

    InputSystem input;
    // В движке UiSystem::attach ставит этот фильтр сам: ImGui → RmlUi → игра.
    input.setEventFilter([&](const InputEvent& e) { return ui.processEvent(e); });
    frame(ui);

    // Кнопка «Пауза» — в правом верхнем углу (pointer-events: auto): щелчок забирает UI.
    Rml::Element* pause = game.document("hud.rml")->GetElementById("pause");
    const Rml::Vector2f p = pause->GetAbsoluteOffset(Rml::BoxArea::Border) + pause->GetBox().GetSize(Rml::BoxArea::Border) * 0.5f;
    input.inject(InputEvent::mouseMove({p.x, p.y}, {0, 0}));
    input.inject(InputEvent::mouseButton(MouseButton::Left, true));
    input.update(1.0 / 60.0);
    frame(ui);
    EXPECT_FALSE(input.mousePressed(MouseButton::Left)) << "игра не видит щелчок по кнопке";
    input.inject(InputEvent::mouseButton(MouseButton::Left, false));
    input.update(1.0 / 60.0);
    frame(ui);
    EXPECT_EQ(pauses, 1);

    // Щелчок в центр экрана (прицел) проходит сквозь HUD в игру.
    input.inject(InputEvent::mouseMove({640, 360}, {0, 0}));
    input.inject(InputEvent::mouseButton(MouseButton::Left, true));
    input.update(1.0 / 60.0);
    EXPECT_TRUE(input.mousePressed(MouseButton::Left));
    input.inject(InputEvent::mouseButton(MouseButton::Left, false));
    input.update(1.0 / 60.0);
}

// Меню настроек графики: модель `settings` поверх ox::Settings + образец settings.rml.
TEST(GuideUiGame, SettingsMenuChangesQuality) {
    render::registerRenderCVars(); // в движке это делает модуль render
    UiSystem ui(gameUiConfig(ui::resourceDir() / "sample"));
    GameUI& game = *ui.gameUI();
    Settings settings; // в движке — engine.settings()
    scalability::setOverall(QualityLevel::High);
    settings.captureFromCVars();

    // В движке хуки заполняет UiSystem::attach по DeviceCaps рендерера; здесь — как на Mac (MoltenVK).
    bool autoRequested = false;
    SettingsMenuHooks hooks;
    hooks.requestAutoDetect = [&] { autoRequested = true; };
    hooks.rayTracingAvailable = [](std::string& reason) {
        reason = "MoltenVK не поддерживает Vulkan ray tracing";
        return false;
    };
    hooks.upscalers = [] {
        return std::vector<UpscalerAvailability>{{"Off", true, {}}, {"FSR1", true, {}}, {"DLSS", false, "нужна NVIDIA RTX"}};
    };
    ASSERT_TRUE(game.bindSettingsMenu(settings, hooks));
    Rml::ElementDocument* doc = game.load("settings.rml", true);
    ASSERT_NE(doc, nullptr);
    frame(ui);
    frame(ui);
    EXPECT_TRUE(doc->GetElementById("quality-High")->IsClassSet("selected"));

    doc->GetElementById("quality-Low")->Click(); // data-event-click="set_quality(q)"
    frame(ui);
    EXPECT_EQ(scalability::overallLevel(), QualityLevel::Low);
    EXPECT_EQ(settings.user().graphics.quality, "Low");

    doc->GetElementById("quality-Auto")->Click(); // бенчмарк запускается рендером между кадрами
    frame(ui);
    EXPECT_TRUE(autoRequested);

    // Недоступные варианты серые, с причиной во всплывающей подсказке; выбрать их нельзя.
    Rml::Element* dlss = doc->GetElementById("upscaler-DLSS");
    EXPECT_TRUE(dlss->IsClassSet("unavailable"));
    EXPECT_EQ(dlss->GetAttribute<Rml::String>("title", ""), "нужна NVIDIA RTX");
    dlss->Click();
    frame(ui);
    EXPECT_NE(settings.user().graphics.upscaler, "DLSS");
    doc->GetElementById("upscaler-FSR1")->Click();
    frame(ui);
    EXPECT_EQ(settings.user().graphics.upscaler, "FSR1");
    EXPECT_TRUE(doc->GetElementById("raytracing")->HasAttribute("disabled"));
    EXPECT_EQ(game.errorCount(), 0u);
    scalability::setOverall(QualityLevel::High);
}

// Все образцы из engine/ui/resources/sample/ загружаются без ошибок RmlUi.
TEST(GuideUiGame, SampleDocumentsLoad) {
    UiSystem ui(gameUiConfig(ui::resourceDir() / "sample"));
    GameUI& game = *ui.gameUI();
    Settings settings;
    ASSERT_TRUE(game.bindSettingsMenu(settings));
    int health = 100, maxHealth = 100, ammo = 30, reserve = 90;
    Rml::String objective = "Найдите выход из лаборатории";
    {
        Rml::DataModelConstructor c = game.createModel("hud");
        c.Bind("health", &health);
        c.Bind("maxHealth", &maxHealth);
        c.Bind("ammo", &ammo);
        c.Bind("reserve", &reserve);
        c.Bind("objective", &objective);
    }
    for (const char* name : {"main_menu.rml", "hud.rml", "settings.rml"}) {
        ASSERT_NE(game.load(name), nullptr) << name;
        EXPECT_FALSE(game.isVisible(name)); // load(path) без show — документ загружен, но скрыт
    }
    EXPECT_TRUE(game.show("main_menu.rml"));
    frame(ui, {1600, 900});
    EXPECT_EQ(game.documents().size(), 3u);
    EXPECT_EQ(game.errorCount(), 0u);
}
