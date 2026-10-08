#include "test_util.hpp"

#include <oxwald/runtime/engine.hpp>
#include <oxwald/runtime/input.hpp>
#include <oxwald/runtime/json_io.hpp>

#if OX_HAS_SCRIPT
#include <oxwald/runtime/script_bindings.hpp>
#include <oxwald/script/script_vm.hpp>
#endif

#include <gtest/gtest.h>

using namespace ox;

namespace {

constexpr f64 kDt = 1.0 / 60.0;

InputContextDesc context(std::string name, i32 priority, std::vector<InputBinding> bindings) {
    return InputContextDesc{std::move(name), priority, std::move(bindings)};
}

InputBinding bind(std::string action, std::string source, std::vector<InputTrigger> triggers = {},
                  std::vector<InputModifier> modifiers = {}) {
    InputBinding b;
    b.action = std::move(action);
    b.source = std::move(source);
    b.triggers = std::move(triggers);
    b.modifiers = std::move(modifiers);
    return b;
}

void press(InputSystem& in, Key k) { in.inject(InputEvent::key(k, true)); }
void release(InputSystem& in, Key k) { in.inject(InputEvent::key(k, false)); }

} // namespace

TEST(Input, SourceNamesRoundTrip) {
    for (const char* name : {"Key.W", "Key.LeftShift", "Key.F12", "Mouse.Left", "Mouse.XY", "Mouse.Wheel",
                             "Gamepad.A", "Gamepad.DpadLeft", "Gamepad.LeftX", "Gamepad.RightTrigger",
                             "Gamepad.LeftStick"}) {
        auto s = parseInputSource(name);
        ASSERT_TRUE(s) << name;
        EXPECT_EQ(inputSourceName(*s), name);
    }
    EXPECT_FALSE(parseInputSource("Key.Nope"));
    EXPECT_FALSE(parseInputSource("Keyboard"));
    EXPECT_EQ(keyFromName("Space"), Key::Space);
    EXPECT_EQ(keyName(Key::Num7), "Num7");
}

TEST(Input, RawStateEdgesAndMouse) {
    InputSystem in;
    press(in, Key::A);
    in.inject(InputEvent::mouseMove({10, 20}, {3, -1}));
    in.inject(InputEvent::mouseMove({12, 21}, {2, 1}));
    in.inject(InputEvent::mouseWheel({0, 1}));
    in.update(kDt);
    EXPECT_TRUE(in.keyDown(Key::A));
    EXPECT_TRUE(in.keyPressed(Key::A));
    EXPECT_EQ(in.mousePosition(), glm::vec2(12, 21));
    EXPECT_EQ(in.mouseDelta(), glm::vec2(5, 0));
    EXPECT_EQ(in.mouseWheel().y, 1.0f);
    in.inject(InputEvent::key(Key::A, true, true)); // OS repeat: ignored
    in.update(kDt);
    EXPECT_TRUE(in.keyDown(Key::A));
    EXPECT_FALSE(in.keyPressed(Key::A));
    EXPECT_EQ(in.mouseDelta(), glm::vec2(0, 0));
    in.inject(InputEvent::focusLost());
    in.update(kDt);
    EXPECT_FALSE(in.keyDown(Key::A));
    EXPECT_TRUE(in.keyReleased(Key::A));
}

TEST(Input, PressedReleasedAndDownTriggers) {
    InputSystem in;
    in.addAction({"Jump", InputValueType::Bool});
    in.addAction({"Fire", InputValueType::Bool});
    in.addAction({"Crouch", InputValueType::Bool});
    in.addContext(context("Default", 0,
                          {bind("Jump", "Key.Space", {InputTrigger::pressed()}),
                           bind("Fire", "Mouse.Left", {InputTrigger::released()}),
                           bind("Crouch", "Key.C")}));
    ASSERT_TRUE(in.activateContext("Default"));

    int jumps = 0;
    auto c = in.bindAction("Jump", ActionEvent::Triggered, [&](const ActionState&) { ++jumps; });
    press(in, Key::Space);
    press(in, Key::C);
    in.inject(InputEvent::mouseButton(MouseButton::Left, true));
    in.update(kDt);
    EXPECT_TRUE(in.triggered("Jump"));
    EXPECT_TRUE(in.triggered("Crouch"));
    EXPECT_FALSE(in.triggered("Fire"));
    EXPECT_EQ(in.action("Fire")->state, TriggerState::Ongoing);
    in.update(kDt); // still held
    EXPECT_FALSE(in.triggered("Jump"));
    EXPECT_TRUE(in.completed("Jump"));
    EXPECT_TRUE(in.triggered("Crouch"));
    in.inject(InputEvent::mouseButton(MouseButton::Left, false));
    release(in, Key::C);
    in.update(kDt);
    EXPECT_TRUE(in.triggered("Fire"));
    EXPECT_FALSE(in.triggered("Crouch"));
    EXPECT_TRUE(in.completed("Crouch"));
    EXPECT_EQ(jumps, 1);

    // Press and release inside one frame still triggers once.
    release(in, Key::Space);
    in.update(kDt);
    press(in, Key::Space);
    release(in, Key::Space);
    in.update(kDt);
    EXPECT_TRUE(in.triggered("Jump"));
    EXPECT_EQ(jumps, 2);
}

TEST(Input, HoldAndTapTriggers) {
    InputSystem in;
    in.addAction({"Interact", InputValueType::Bool});
    in.addAction({"Dodge", InputValueType::Bool});
    in.addContext(context("Default", 0,
                          {bind("Interact", "Key.E", {InputTrigger::hold(0.5f)}),
                           bind("Dodge", "Key.Q", {InputTrigger::tap(0.2f)})}));
    in.activateContext("Default");

    int holds = 0, canceled = 0;
    auto c1 = in.bindAction("Interact", ActionEvent::Triggered, [&](const ActionState&) { ++holds; });
    auto c2 = in.bindAction("Interact", ActionEvent::Canceled, [&](const ActionState&) { ++canceled; });

    // Hold 0.3 s then release: started + canceled, never triggered.
    press(in, Key::E);
    for (int i = 0; i < 18; ++i) in.update(kDt);
    EXPECT_EQ(in.action("Interact")->state, TriggerState::Ongoing);
    release(in, Key::E);
    in.update(kDt);
    EXPECT_EQ(holds, 0);
    EXPECT_EQ(canceled, 1);

    // Hold 1 s: triggers exactly once (one-shot) after 0.5 s.
    press(in, Key::E);
    int frameTriggered = -1;
    for (int i = 0; i < 60; ++i) {
        in.update(kDt);
        if (in.triggered("Interact") && frameTriggered < 0) frameTriggered = i;
    }
    EXPECT_EQ(holds, 1);
    EXPECT_GE(frameTriggered, 29);
    EXPECT_LE(frameTriggered, 31);
    release(in, Key::E);
    in.update(kDt);

    // Tap: quick press/release triggers on release; long press does not.
    press(in, Key::Q);
    in.update(kDt);
    in.update(kDt);
    release(in, Key::Q);
    in.update(kDt);
    EXPECT_TRUE(in.triggered("Dodge"));
    press(in, Key::Q);
    for (int i = 0; i < 30; ++i) in.update(kDt);
    release(in, Key::Q);
    in.update(kDt);
    EXPECT_FALSE(in.triggered("Dodge"));
}

TEST(Input, ChordRequiresOtherAction) {
    InputSystem in;
    in.addAction({"Modifier", InputValueType::Bool});
    in.addAction({"QuickSave", InputValueType::Bool});
    // Chord declared before its dependency to exercise resolution order.
    in.addContext(context("Default", 0,
                          {bind("QuickSave", "Key.S", {InputTrigger::pressed(), InputTrigger::chord("Modifier")}),
                           bind("Modifier", "Key.LeftControl")}));
    in.activateContext("Default");
    press(in, Key::S);
    in.update(kDt);
    EXPECT_FALSE(in.triggered("QuickSave"));
    release(in, Key::S);
    in.update(kDt);
    press(in, Key::LeftControl);
    press(in, Key::S);
    in.update(kDt);
    EXPECT_TRUE(in.triggered("QuickSave"));
}

TEST(Input, DeadZonesAndAxisModifiers) {
    InputSystem in;
    in.addAction({"Move", InputValueType::Axis2D});
    in.addAction({"Look", InputValueType::Axis2D});
    in.addAction({"Throttle", InputValueType::Axis1D});
    // WASD as a 2D axis: D = +X, A = -X, W = +Y (swizzle), S = -Y (negate + swizzle).
    in.addContext(context(
        "Default", 0,
        {bind("Move", "Key.D"), bind("Move", "Key.A", {}, {InputModifier::makeNegate()}),
         bind("Move", "Key.W", {}, {InputModifier::makeSwizzle()}),
         bind("Move", "Key.S", {}, {InputModifier::makeNegate(), InputModifier::makeSwizzle()}),
         bind("Move", "Gamepad.LeftStick", {}, {InputModifier::makeDeadZone(0.2f, 1.0f)}),
         bind("Look", "Mouse.XY", {}, {InputModifier::makeScale({0.5f, -0.5f, 1.0f})}),
         bind("Throttle", "Gamepad.RightTrigger", {}, {InputModifier::makeDeadZone(0.1f, 0.9f, DeadZoneKind::Axial)})}));
    in.activateContext("Default");

    press(in, Key::W);
    press(in, Key::D);
    in.update(kDt);
    EXPECT_EQ(in.axis2D("Move"), glm::vec2(1, 1));
    release(in, Key::D);
    press(in, Key::S);
    in.update(kDt);
    // HighestAbsolute: W (+1) and S (-1) tie; first binding wins on ties -> components keep the larger magnitude.
    EXPECT_EQ(std::abs(in.axis2D("Move").y), 1.0f);
    release(in, Key::W);
    release(in, Key::S);

    in.inject(InputEvent::gamepadAxis(0, GamepadAxis::LeftX, 0.15f));
    in.inject(InputEvent::gamepadAxis(0, GamepadAxis::LeftY, 0.0f));
    in.update(kDt);
    EXPECT_EQ(in.axis2D("Move"), glm::vec2(0, 0)); // inside the radial dead zone
    EXPECT_FALSE(in.action("Move")->active());
    in.inject(InputEvent::gamepadAxis(0, GamepadAxis::LeftX, 0.6f));
    in.update(kDt);
    EXPECT_NEAR(in.axis2D("Move").x, 0.5f, 1e-5f); // (0.6-0.2)/(1-0.2)
    EXPECT_TRUE(in.action("Move")->active());

    in.inject(InputEvent::gamepadAxis(0, GamepadAxis::RightTrigger, 0.95f));
    in.inject(InputEvent::mouseMove({0, 0}, {4, 2}));
    in.update(kDt);
    EXPECT_FLOAT_EQ(in.axis1D("Throttle"), 1.0f);
    EXPECT_EQ(in.axis2D("Look"), glm::vec2(2, -1));
    in.update(kDt);
    EXPECT_EQ(in.axis2D("Look"), glm::vec2(0, 0)); // deltas are per frame
}

TEST(Input, CumulativeAccumulation) {
    InputSystem in;
    in.addAction({"Strafe", InputValueType::Axis1D, InputAccumulation::Cumulative});
    in.addContext(context("Default", 0,
                          {bind("Strafe", "Key.D"), bind("Strafe", "Key.A", {}, {InputModifier::makeNegate()})}));
    in.activateContext("Default");
    press(in, Key::A);
    press(in, Key::D);
    in.update(kDt);
    EXPECT_FLOAT_EQ(in.axis1D("Strafe"), 0.0f);
}

TEST(Input, ContextPriorityConsumesSources) {
    InputSystem in;
    in.addAction({"Jump", InputValueType::Bool});
    in.addAction({"Accelerate", InputValueType::Bool});
    in.addAction({"Pause", InputValueType::Bool});
    in.addContext(context("OnFoot", 0, {bind("Jump", "Key.Space"), bind("Pause", "Key.Escape")}));
    in.addContext(context("Vehicle", 10, {bind("Accelerate", "Key.Space")}));
    in.activateContext("OnFoot");
    in.activateContext("Vehicle");
    EXPECT_EQ(in.activeContexts(), (std::vector<std::string>{"Vehicle", "OnFoot"}));

    press(in, Key::Space);
    press(in, Key::Escape);
    in.update(kDt);
    EXPECT_TRUE(in.triggered("Accelerate"));
    EXPECT_FALSE(in.triggered("Jump")); // consumed by the higher priority context
    EXPECT_TRUE(in.triggered("Pause"));  // not bound by Vehicle

    in.deactivateContext("Vehicle");
    in.update(kDt);
    EXPECT_TRUE(in.triggered("Jump"));
    EXPECT_FALSE(in.triggered("Accelerate"));

    // Priority override on activation.
    in.activateContext("Vehicle", -5);
    in.update(kDt);
    EXPECT_TRUE(in.triggered("Jump"));
    EXPECT_FALSE(in.triggered("Accelerate"));
}

TEST(Input, RebindingAndCapture) {
    InputSystem in;
    in.addAction({"Jump", InputValueType::Bool});
    in.addContext(context("Default", 0, {bind("Jump", "Key.Space"), bind("Jump", "Gamepad.A")}));
    in.activateContext("Default");
    int changed = 0;
    auto c = in.rebindsChanged.connect([&] { ++changed; });

    EXPECT_FALSE(in.rebind("Default", "Jump", 0, "Key.Bogus"));
    EXPECT_FALSE(in.rebind("Default", "Jump", 5, "Key.J"));
    ASSERT_TRUE(in.rebind("Default", "Jump", 0, "Key.J"));
    EXPECT_EQ(in.bindingSource("Default", "Jump", 0), "Key.J");
    EXPECT_EQ(in.bindingSource("Default", "Jump", 1), "Gamepad.A");
    press(in, Key::Space);
    in.update(kDt);
    EXPECT_FALSE(in.triggered("Jump"));
    press(in, Key::J);
    in.update(kDt);
    EXPECT_TRUE(in.triggered("Jump"));
    EXPECT_EQ(in.rebinds().at("Default/Jump/0"), "Key.J");

    // Capture: the next pressed input is reported (and does not fire actions).
    std::string captured;
    in.captureNextInput([&](const std::string& s) { captured = s; });
    in.inject(InputEvent::gamepadButton(0, GamepadButton::Y, true));
    in.update(kDt);
    EXPECT_EQ(captured, "Gamepad.Y");
    EXPECT_FALSE(in.capturing());
    ASSERT_TRUE(in.rebind("Default", "Jump", 1, captured));

    in.resetBinding("Default", "Jump", 0);
    EXPECT_EQ(in.bindingSource("Default", "Jump", 0), "Key.Space");
    EXPECT_EQ(changed, 3);
}

TEST(Input, RebindsPersistInUserSettings) {
    test::TempDir dir;
    ProjectSettings project;
    project.input.actions = {{"Jump", InputValueType::Bool}};
    project.input.contexts = {context("Default", 0, {bind("Jump", "Key.Space")})};
    project.input.activeContexts = {"Default"};
    EngineConfig cfg;
    cfg.headless = true;
    cfg.workerThreads = 2;
    cfg.userDir = dir / "user";
    cfg.projectSettings = project;
    {
        Engine engine;
        ASSERT_TRUE(engine.init(cfg));
        ASSERT_TRUE(engine.input().rebind("Default", "Jump", 0, "Key.K"));
        EXPECT_EQ(engine.settings().user().inputRebinds.at("Default/Jump/0"), "Key.K");
    } // shutdown saves user://settings.json
    ASSERT_TRUE(std::filesystem::exists(dir / "user/settings.json"));
    Engine engine;
    ASSERT_TRUE(engine.init(cfg));
    EXPECT_EQ(engine.input().bindingSource("Default", "Jump", 0), "Key.K");
    engine.input().inject(InputEvent::key(Key::K, true));
    engine.tick(kDt);
    EXPECT_TRUE(engine.input().triggered("Jump"));
}

TEST(Input, MappingsSerializeToPlainJson) {
    registerInputTypes();
    InputMappingConfig cfg;
    cfg.actions = {{"Move", InputValueType::Axis2D, InputAccumulation::Cumulative, "walk"}};
    cfg.contexts = {context("Default", 3,
                            {bind("Move", "Key.W", {InputTrigger::hold(0.3f, false)},
                                  {InputModifier::makeSwizzle(SwizzleOrder::ZXY), InputModifier::makeScale({2, 3, 4})})})};
    cfg.activeContexts = {"Default"};
    const auto j = json::toPlain(cfg);
    EXPECT_EQ(j["contexts"][0]["bindings"][0]["triggers"][0]["type"], "Hold");
    EXPECT_EQ(j.dump().find("$type"), std::string::npos);
    InputMappingConfig back;
    ASSERT_TRUE(json::fromPlain(j, back));
    ASSERT_EQ(back.contexts.size(), 1u);
    const auto& b = back.contexts[0].bindings[0];
    EXPECT_EQ(back.contexts[0].priority, 3);
    EXPECT_EQ(b.triggers[0].type, InputTriggerType::Hold);
    EXPECT_FLOAT_EQ(b.triggers[0].time, 0.3f);
    EXPECT_FALSE(b.triggers[0].oneShot);
    EXPECT_EQ(b.modifiers[0].order, SwizzleOrder::ZXY);
    EXPECT_EQ(b.modifiers[1].vector, glm::vec3(2, 3, 4));
    EXPECT_EQ(back.actions[0].accumulation, InputAccumulation::Cumulative);
}

#if OX_HAS_SCRIPT
TEST(Input, LuaInputTable) {
    InputSystem in;
    in.addAction({"Move", InputValueType::Axis2D});
    in.addAction({"Jump", InputValueType::Bool});
    in.addContext(context("Default", 0, {bind("Move", "Key.D"), bind("Jump", "Key.Space", {InputTrigger::pressed()})}));
    in.activateContext("Default");
    script::ScriptVM vm;
    bindInputLuaApi(vm, in);
    press(in, Key::D);
    press(in, Key::Space);
    in.update(kDt);
    auto env = vm.createEnvironment();
    auto r = vm.runString("local m = input.action('Move'); return input.keyDown('D') and input.triggered('Jump') and m.x == 1",
                          "test", &env);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_TRUE(r.value.as<bool>());
}
#endif
