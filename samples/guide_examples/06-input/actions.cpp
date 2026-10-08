// Глава 06: действия, контексты, модификаторы, триггеры, ребинд (docs/guide/06-input.md).
#include <oxwald/runtime/engine.hpp>
#include <oxwald/runtime/input.hpp>
#include <oxwald/runtime/json_io.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr double kDt = 1.0 / 60.0;

// Раскладка «пешком»: WASD + стик как 2D-ось, прыжок, взаимодействие удержанием, быстрое сохранение по Ctrl+S.
ox::InputMappingConfig makeMappings() {
    using ox::InputModifier;
    using ox::InputTrigger;
    ox::InputMappingConfig m;
    m.actions = {
        {"Move", ox::InputValueType::Axis2D},
        {"Look", ox::InputValueType::Axis2D},
        {"Jump", ox::InputValueType::Bool},
        {"Interact", ox::InputValueType::Bool},
        {"Modifier", ox::InputValueType::Bool},
        {"QuickSave", ox::InputValueType::Bool},
    };
    m.contexts = {{"OnFoot", 0, {
        // WASD -> 2D: D = +X, A = -X, W = +Y (swizzle X->Y), S = -Y.
        {"Move", "Key.D"},
        {"Move", "Key.A", {InputModifier::makeNegate()}},
        {"Move", "Key.W", {InputModifier::makeSwizzle()}},
        {"Move", "Key.S", {InputModifier::makeNegate(), InputModifier::makeSwizzle()}},
        {"Move", "Gamepad.LeftStick", {InputModifier::makeDeadZone(0.2f)}},
        // Мышь: инвертируем Y и масштабируем чувствительность.
        {"Look", "Mouse.XY", {InputModifier::makeScale({0.1f, -0.1f, 1.0f})}},
        {"Jump", "Key.Space", {}, {InputTrigger::pressed()}},
        {"Jump", "Gamepad.A", {}, {InputTrigger::pressed()}},
        {"Interact", "Key.E", {}, {InputTrigger::hold(0.5f)}},
        {"Modifier", "Key.LeftControl"},
        {"QuickSave", "Key.S", {}, {InputTrigger::pressed(), InputTrigger::chord("Modifier")}},
    }}};
    m.activeContexts = {"OnFoot"};
    return m;
}

} // namespace

TEST(GuideInput, AxesAndTriggers) {
    ox::InputSystem input;
    input.setMappings(makeMappings());

    // Платформа (GLFW, вьюпорт редактора, тест) кладёт события из любого потока...
    input.inject(ox::InputEvent::key(ox::Key::W, true));
    input.inject(ox::InputEvent::key(ox::Key::D, true));
    input.inject(ox::InputEvent::key(ox::Key::Space, true));
    input.inject(ox::InputEvent::mouseMove({100, 100}, {20, 10}));
    // ...а игровой поток раз в кадр их применяет (Engine делает это сам).
    input.update(kDt);

    EXPECT_EQ(input.axis2D("Move"), glm::vec2(1, 1));
    EXPECT_TRUE(ox::nearlyEqual(input.axis2D("Look"), glm::vec2(2, -1), 1e-5f));
    EXPECT_TRUE(input.triggered("Jump"));

    input.update(kDt); // пробел всё ещё зажат, но Pressed срабатывает один раз
    EXPECT_FALSE(input.triggered("Jump"));
    EXPECT_EQ(input.axis2D("Look"), glm::vec2(0, 0)); // дельты мыши — покадровые
}

TEST(GuideInput, HoldChordAndCallbacks) {
    ox::InputSystem input;
    input.setMappings(makeMappings());

    int interactions = 0;
    ox::ScopedConnection conn = input.bindAction("Interact", ox::ActionEvent::Triggered,
                                                 [&](const ox::ActionState&) { ++interactions; });
    int canceled = 0;
    ox::ScopedConnection conn2 = input.bindAction("Interact", ox::ActionEvent::Canceled,
                                                  [&](const ox::ActionState&) { ++canceled; });

    // Отпустили раньше 0.5 с — Canceled (можно спрятать индикатор удержания).
    input.inject(ox::InputEvent::key(ox::Key::E, true));
    for (int i = 0; i < 10; ++i) input.update(kDt);
    EXPECT_EQ(input.action("Interact")->state, ox::TriggerState::Ongoing);
    input.inject(ox::InputEvent::key(ox::Key::E, false));
    input.update(kDt);
    EXPECT_EQ(canceled, 1);

    // Удерживаем секунду — Triggered ровно один раз (oneShot).
    input.inject(ox::InputEvent::key(ox::Key::E, true));
    for (int i = 0; i < 60; ++i) input.update(kDt);
    EXPECT_EQ(interactions, 1);
    input.inject(ox::InputEvent::key(ox::Key::E, false));
    input.update(kDt);

    // Chord: S без Ctrl — только движение назад, Ctrl+S — быстрое сохранение.
    input.inject(ox::InputEvent::key(ox::Key::S, true));
    input.update(kDt);
    EXPECT_FALSE(input.triggered("QuickSave"));
    input.inject(ox::InputEvent::key(ox::Key::S, false));
    input.update(kDt);
    input.inject(ox::InputEvent::key(ox::Key::LeftControl, true));
    input.inject(ox::InputEvent::key(ox::Key::S, true));
    input.update(kDt);
    EXPECT_TRUE(input.triggered("QuickSave"));
}

TEST(GuideInput, ContextPriority) {
    ox::InputSystem input;
    input.setMappings(makeMappings());
    // Контекст «в машине» поверх «пешком»: пробел теперь — тормоз, а не прыжок.
    input.addAction({"Brake", ox::InputValueType::Bool});
    input.addContext({"Vehicle", 10, {{"Brake", "Key.Space"}}});
    input.activateContext("Vehicle");
    EXPECT_EQ(input.activeContexts(), (std::vector<std::string>{"Vehicle", "OnFoot"}));

    input.inject(ox::InputEvent::key(ox::Key::Space, true));
    input.update(kDt);
    EXPECT_TRUE(input.triggered("Brake"));
    EXPECT_FALSE(input.triggered("Jump")); // источник «съеден» контекстом с большим приоритетом

    input.deactivateContext("Vehicle"); // вышли из машины
    input.inject(ox::InputEvent::key(ox::Key::Space, false));
    input.update(kDt);
    input.inject(ox::InputEvent::key(ox::Key::Space, true));
    input.update(kDt);
    EXPECT_TRUE(input.triggered("Jump"));
}

TEST(GuideInput, RebindingMenu) {
    ox::InputSystem input;
    input.setMappings(makeMappings());

    // Экран настроек: «нажмите новую клавишу для прыжка».
    std::string captured;
    input.captureNextInput([&](const std::string& source) { captured = source; });
    input.inject(ox::InputEvent::key(ox::Key::J, true));
    input.update(kDt); // захваченный ввод не запускает действия
    EXPECT_EQ(captured, "Key.J");

    // Binding #0 действия Jump в контексте OnFoot (Key.Space) -> Key.J.
    ASSERT_TRUE(input.rebind("OnFoot", "Jump", 0, captured));
    EXPECT_EQ(input.bindingSource("OnFoot", "Jump", 0), "Key.J");
    EXPECT_EQ(input.rebinds().at("OnFoot/Jump/0"), "Key.J"); // это и сохраняется в user://settings.json

    input.resetAllBindings();
    EXPECT_EQ(input.bindingSource("OnFoot", "Jump", 0), "Key.Space");
}

TEST(GuideInput, ProjectMappingsAndPersistedRebinds) {
    const auto dir = std::filesystem::temp_directory_path() / ("oxwald_guide_input_" + ox::Uuid::generate().toString());
    ox::ProjectSettings project;
    project.input = makeMappings(); // в реальном проекте — секция "input" файла .oxproj

    ox::EngineConfig config;
    config.headless = true;
    config.workerThreads = 2;
    config.userDir = dir;
    config.projectSettings = project;
    {
        ox::Engine engine;
        ASSERT_TRUE(engine.init(config));
        ASSERT_TRUE(engine.input().rebind("OnFoot", "Jump", 0, "Key.K"));
    } // при shutdown ребинды пишутся в user://settings.json

    ox::Engine engine;
    ASSERT_TRUE(engine.init(config));
    EXPECT_EQ(engine.input().bindingSource("OnFoot", "Jump", 0), "Key.K");
    engine.input().inject(ox::InputEvent::key(ox::Key::K, true));
    engine.tick(kDt); // Engine::tick вызывает input.update()
    EXPECT_TRUE(engine.input().triggered("Jump"));
    engine.shutdown();

    // Раскладка — обычные отражённые структуры: так она выглядит в .oxproj.
    ox::registerInputTypes();
    const auto json = ox::json::toPlain(project.input);
    EXPECT_EQ(json["contexts"][0]["bindings"][0]["source"], "Key.D");
    std::filesystem::remove_all(dir);
}
