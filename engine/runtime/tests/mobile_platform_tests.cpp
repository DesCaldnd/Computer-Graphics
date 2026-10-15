// Platform-agnostic parts of the Android port: touch -> Enhanced Input mapping, the app lifecycle state machine
// (surface create/destroy on pause/resume), Android key code / gamepad axis tables.
#include <oxwald/runtime/android_keymap.hpp>
#include <oxwald/runtime/app_lifecycle.hpp>
#include <oxwald/runtime/input.hpp>
#include <oxwald/runtime/launch.hpp>
#include <oxwald/runtime/touch_input.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace ox;

namespace {

constexpr glm::vec2 kScreen{2000.0f, 1000.0f};

InputMappingConfig mobileMappings() {
    InputMappingConfig c;
    c.actions = {{"Move", InputValueType::Axis2D}, {"Look", InputValueType::Axis2D}, {"Jump", InputValueType::Bool},
                 {"Click", InputValueType::Bool}, {"Zoom", InputValueType::Axis1D}};
    InputContextDesc ctx;
    ctx.name = "Game";
    ctx.bindings = {{"Move", "Gamepad.LeftStick", {}, {}, true},
                    {"Look", "Mouse.XY", {}, {}, true},
                    {"Jump", "Gamepad.A", {}, {}, true},
                    {"Click", "Mouse.Left", {}, {}, true},
                    {"Zoom", "Mouse.Wheel", {}, {}, true}};
    c.contexts = {ctx};
    c.activeContexts = {"Game"};
    return c;
}

struct Rig {
    InputSystem input;
    TouchInputMapper touch;
    std::vector<InputEvent> events;
    f64 time = 0.0;
    Rig() {
        input.setMappings(mobileMappings());
        touch.setViewport(kScreen);
    }
    void send(TouchPhase phase, i32 id, glm::vec2 pos, f64 dt = 0.016) {
        time += dt;
        touch.process({phase, id, pos, time}, events);
    }
    void frame() {
        for (const InputEvent& e : events) input.inject(e);
        events.clear();
        input.update(1.0 / 60.0);
    }
};

usize count(const std::vector<InputEvent>& ev, InputEvent::Type t) {
    return usize(std::count_if(ev.begin(), ev.end(), [&](const InputEvent& e) { return e.type == t; }));
}

} // namespace

// ---- touch ----------------------------------------------------------------------------------------------------

TEST(TouchInput, LeftHalfIsAFloatingStickOnTheVirtualPad) {
    Rig r;
    r.send(TouchPhase::Down, 1, {300, 600});
    EXPECT_TRUE(r.touch.stickActive());
    // Full deflection = stickRadius * height = 120 px; 60 px right and 120 px up -> (0.5, 1) clamped to length 1.
    r.send(TouchPhase::Move, 1, {360, 480});
    r.frame();
    const glm::vec2 move = r.input.axis2D("Move");
    EXPECT_GT(move.x, 0.3f);
    EXPECT_GT(move.y, 0.8f) << "screen up must be stick +Y";
    EXPECT_LE(glm::length(r.touch.stickValue()), 1.0001f);
    EXPECT_TRUE(r.input.gamepadConnected(TouchLayout{}.gamepadSlot));
    EXPECT_FALSE(r.input.gamepadConnected(0)) << "slot 0 stays free for a real controller";

    r.send(TouchPhase::Up, 1, {360, 480});
    r.frame();
    EXPECT_EQ(r.input.axis2D("Move"), glm::vec2(0.0f));
    EXPECT_FALSE(r.touch.stickActive());
}

TEST(TouchInput, StickDeadZoneAndSecondFingerInMoveZoneLooks) {
    Rig r;
    r.send(TouchPhase::Down, 1, {300, 600});
    r.send(TouchPhase::Move, 1, {305, 600}); // 5 px of 120 -> inside the dead zone
    EXPECT_EQ(r.touch.stickValue(), glm::vec2(0.0f));
    r.events.clear();
    r.send(TouchPhase::Down, 2, {200, 300}); // stick already taken: this finger looks
    r.send(TouchPhase::Move, 2, {230, 290});
    EXPECT_EQ(count(r.events, InputEvent::Type::MouseMove), 2u);
    r.frame();
    EXPECT_EQ(r.input.mouseDelta(), glm::vec2(30.0f, -10.0f));
}

TEST(TouchInput, RightHalfDragDrivesMouseLook) {
    Rig r;
    r.send(TouchPhase::Down, 7, {1500, 400});
    r.send(TouchPhase::Move, 7, {1520, 410});
    r.frame();
    EXPECT_EQ(r.input.mouseDelta(), glm::vec2(20.0f, 10.0f));
    EXPECT_EQ(r.input.mousePosition(), glm::vec2(1520.0f, 410.0f));
    EXPECT_NE(r.input.axis2D("Look"), glm::vec2(0.0f));
    r.send(TouchPhase::Up, 7, {1520, 410}, 1.0); // long drag: no click
    r.frame();
    EXPECT_FALSE(r.input.mouseDown(MouseButton::Left));
    EXPECT_FALSE(r.input.mouseReleased(MouseButton::Left));
}

TEST(TouchInput, ShortTouchIsAClickAtTheTouchPosition) {
    Rig r;
    r.send(TouchPhase::Down, 3, {1200, 300});
    r.send(TouchPhase::Up, 3, {1203, 301}, 0.1);
    EXPECT_EQ(count(r.events, InputEvent::Type::MouseButton), 2u);
    r.frame();
    EXPECT_TRUE(r.input.mousePressed(MouseButton::Left));
    EXPECT_TRUE(r.input.mouseReleased(MouseButton::Left));
    EXPECT_EQ(r.input.mousePosition(), glm::vec2(1200.0f, 300.0f));
}

TEST(TouchInput, ButtonsPressTheirSourcesWhileHeld) {
    Rig r;
    // Default Jump button: x 84..98 %, y 70..95 %.
    r.send(TouchPhase::Down, 4, {0.9f * kScreen.x, 0.8f * kScreen.y});
    r.frame();
    EXPECT_TRUE(r.input.triggered("Jump"));
    EXPECT_TRUE(r.input.keyDown(Key::Space));
    r.send(TouchPhase::Move, 4, {0.9f * kScreen.x, 0.85f * kScreen.y});
    r.frame();
    EXPECT_TRUE(r.input.keyDown(Key::Space));
    EXPECT_EQ(r.input.mouseDelta(), glm::vec2(0.0f)) << "button fingers do not look around";
    r.send(TouchPhase::Up, 4, {0.9f * kScreen.x, 0.85f * kScreen.y});
    r.frame();
    EXPECT_FALSE(r.input.keyDown(Key::Space));
    EXPECT_FALSE(r.input.triggered("Jump"));
}

TEST(TouchInput, PinchScrollsTheWheel) {
    Rig r;
    r.send(TouchPhase::Down, 1, {1200, 500});
    r.send(TouchPhase::Down, 2, {1400, 500});
    r.events.clear();
    r.send(TouchPhase::Move, 2, {1500, 500}); // distance 200 -> 300
    ASSERT_EQ(count(r.events, InputEvent::Type::MouseWheel), 1u);
    EXPECT_EQ(count(r.events, InputEvent::Type::MouseMove), 0u) << "pinching does not look";
    r.frame();
    EXPECT_GT(r.input.axis1D("Zoom"), 0.0f);
    r.send(TouchPhase::Up, 2, {1500, 500}, 0.05);
    EXPECT_EQ(count(r.events, InputEvent::Type::MouseButton), 0u) << "the end of a pinch is no tap";
}

TEST(TouchInput, ResetReleasesEverythingAndCancelIsNoTap) {
    Rig r;
    r.send(TouchPhase::Down, 1, {300, 600});
    r.send(TouchPhase::Move, 1, {400, 600});
    r.send(TouchPhase::Down, 2, {0.9f * kScreen.x, 0.8f * kScreen.y});
    r.frame();
    ASSERT_TRUE(r.input.keyDown(Key::Space));
    r.touch.reset(r.events);
    r.frame();
    EXPECT_FALSE(r.input.keyDown(Key::Space));
    EXPECT_EQ(r.input.axis2D("Move"), glm::vec2(0.0f));
    EXPECT_EQ(r.touch.activePointers(), 0u);

    r.send(TouchPhase::Down, 5, {1500, 300});
    r.send(TouchPhase::Cancel, 5, {1500, 300}, 0.05);
    EXPECT_EQ(count(r.events, InputEvent::Type::MouseButton), 0u);
}

TEST(TouchInput, CustomLayoutSourcesAreParsed) {
    TouchLayout l;
    l.moveZone = {0, 0, 0, 0}; // no stick
    l.buttons = {{"Fire", {0, 0, 1, 0.2f}, {"Mouse.Right", "Gamepad.RightTrigger", "Key.Bogus"}}};
    TouchInputMapper m(l);
    m.setViewport(kScreen);
    std::vector<InputEvent> ev;
    m.process({TouchPhase::Down, 1, {100, 50}, 0.0}, ev);
    // pad connected + trigger axis + right mouse button; the unknown source is ignored.
    EXPECT_EQ(count(ev, InputEvent::Type::MouseButton), 1u);
    EXPECT_EQ(count(ev, InputEvent::Type::GamepadAxis), 1u);
    EXPECT_EQ(count(ev, InputEvent::Type::GamepadConnected), 1u);
    ev.clear();
    m.process({TouchPhase::Down, 2, {300, 600}, 0.0}, ev); // outside the empty move zone -> look
    EXPECT_FALSE(m.stickActive());
    EXPECT_EQ(count(ev, InputEvent::Type::MouseMove), 1u);
}

// ---- lifecycle ------------------------------------------------------------------------------------------------

TEST(AppLifecycle, ColdStartCreatesSurfaceAndRendersOnceResumedWithWindow) {
    AppLifecycle l;
    EXPECT_FALSE(l.handle(AppEvent::Start).any());
    EXPECT_FALSE(l.shouldRender());
    l.handle(AppEvent::Resume);
    EXPECT_FALSE(l.shouldRender()) << "no window yet";
    const LifecycleActions a = l.handle(AppEvent::WindowCreated);
    EXPECT_TRUE(a.createSurface);
    EXPECT_FALSE(a.destroySurface);
    EXPECT_TRUE(l.shouldRender());
    EXPECT_FALSE(l.shouldSimulate()) << "no focus yet";
    const LifecycleActions f = l.handle(AppEvent::FocusGained);
    EXPECT_TRUE(l.shouldSimulate());
    EXPECT_FALSE(f.resumeAudio) << "audio was never paused";
    EXPECT_EQ(l.windowGeneration(), 1u);
}

TEST(AppLifecycle, PauseThenWindowLossAndResumeRecreateTheSurface) {
    AppLifecycle l;
    for (AppEvent e : {AppEvent::Start, AppEvent::Resume, AppEvent::WindowCreated, AppEvent::FocusGained}) l.handle(e);
    const LifecycleActions lost = l.handle(AppEvent::FocusLost);
    EXPECT_TRUE(lost.releaseInput);
    EXPECT_TRUE(lost.pauseAudio);
    const LifecycleActions pause = l.handle(AppEvent::Pause);
    EXPECT_TRUE(pause.saveState);
    EXPECT_FALSE(pause.pauseAudio) << "already paused";
    EXPECT_TRUE(l.shouldRender()) << "paused but visible (multi-window) still renders";
    const LifecycleActions gone = l.handle(AppEvent::WindowDestroyed);
    EXPECT_TRUE(gone.destroySurface);
    EXPECT_FALSE(l.shouldRender());
    EXPECT_TRUE(l.handle(AppEvent::Stop).saveState);
    EXPECT_FALSE(l.handle(AppEvent::WindowDestroyed).any()) << "duplicate destroy is a no-op";

    l.handle(AppEvent::Start);
    l.handle(AppEvent::Resume);
    const LifecycleActions back = l.handle(AppEvent::WindowCreated);
    EXPECT_TRUE(back.createSurface);
    EXPECT_FALSE(back.resumeAudio) << "still unfocused";
    const LifecycleActions focus = l.handle(AppEvent::FocusGained);
    EXPECT_TRUE(focus.resumeAudio);
    EXPECT_TRUE(l.shouldSimulate());
    EXPECT_EQ(l.windowGeneration(), 3u); // created, destroyed, created
}

TEST(AppLifecycle, WindowDestroyedBeforePauseAndReplacementWindow) {
    AppLifecycle l;
    for (AppEvent e : {AppEvent::Resume, AppEvent::FocusGained, AppEvent::WindowCreated}) l.handle(e);
    ASSERT_TRUE(l.shouldSimulate());
    const LifecycleActions gone = l.handle(AppEvent::WindowDestroyed); // some devices: before onPause
    EXPECT_TRUE(gone.destroySurface);
    EXPECT_TRUE(gone.releaseInput);
    EXPECT_TRUE(gone.pauseAudio);
    l.handle(AppEvent::WindowCreated);
    const LifecycleActions replaced = l.handle(AppEvent::WindowCreated); // new window without destroy
    EXPECT_TRUE(replaced.destroySurface);
    EXPECT_TRUE(replaced.createSurface);
}

TEST(AppLifecycle, RotationResizesAndDestroyQuits) {
    AppLifecycle l;
    EXPECT_FALSE(l.handle(AppEvent::ConfigChanged).recreateSwapchain) << "no window, nothing to resize";
    for (AppEvent e : {AppEvent::Start, AppEvent::Resume, AppEvent::WindowCreated, AppEvent::FocusGained}) l.handle(e);
    EXPECT_TRUE(l.handle(AppEvent::ConfigChanged).recreateSwapchain);
    EXPECT_TRUE(l.handle(AppEvent::WindowResized).recreateSwapchain);
    EXPECT_TRUE(l.handle(AppEvent::LowMemory).trimMemory);
    const LifecycleActions d = l.handle(AppEvent::Destroy);
    EXPECT_TRUE(d.quit);
    EXPECT_TRUE(d.destroySurface);
    EXPECT_TRUE(d.saveState);
    EXPECT_TRUE(d.pauseAudio);
    EXPECT_TRUE(l.destroyed());
    EXPECT_FALSE(l.handle(AppEvent::Resume).any());
    EXPECT_FALSE(l.shouldRender());
    EXPECT_EQ(LifecycleActions{}.toString(), "none");
    EXPECT_EQ(appEventName(AppEvent::WindowCreated), "WindowCreated");
}

// ---- intent extra command lines --------------------------------------------------------------------------------

TEST(LaunchCommandLine, SplitsQuotesAndEscapes) {
    EXPECT_EQ(splitCommandLine("  --frames 300\t--cvar r.Vsync=0 "),
              (std::vector<std::string>{"--frames", "300", "--cvar", "r.Vsync=0"}));
    EXPECT_EQ(splitCommandLine("--scene \"project://levels/my hub.oxscene\" --x 'a b' c\\ d"),
              (std::vector<std::string>{"--scene", "project://levels/my hub.oxscene", "--x", "a b", "c d"}));
    EXPECT_EQ(splitCommandLine("\"\" x"), (std::vector<std::string>{"", "x"}));
    EXPECT_TRUE(splitCommandLine("   ").empty());
    auto opts = parseLaunchOptions(splitCommandLine("--frames 120 --quality low -- --game-flag"));
    ASSERT_TRUE(opts);
    EXPECT_EQ(opts->frames, 120u);
    EXPECT_EQ(opts->gameArgs, (std::vector<std::string>{"--game-flag"}));
}

// ---- Android key codes / axes ----------------------------------------------------------------------------------

TEST(AndroidKeymap, KeyboardAndGamepadCodes) {
    using namespace android_input;
    EXPECT_EQ(keyFromKeycode(29), Key::A);
    EXPECT_EQ(keyFromKeycode(54), Key::Z);
    EXPECT_EQ(keyFromKeycode(7), Key::Num0);
    EXPECT_EQ(keyFromKeycode(142), Key::F12);
    EXPECT_EQ(keyFromKeycode(62), Key::Space);
    EXPECT_EQ(keyFromKeycode(kKeycodeBack), Key::Escape);
    EXPECT_EQ(keyFromKeycode(153), Key::Kp9);
    EXPECT_EQ(keyFromKeycode(96), std::nullopt) << "gamepad buttons are not keyboard keys";
    EXPECT_EQ(gamepadButtonFromKeycode(96), GamepadButton::A);
    EXPECT_EQ(gamepadButtonFromKeycode(100), GamepadButton::Y);
    EXPECT_EQ(gamepadButtonFromKeycode(108), GamepadButton::Start);
    EXPECT_EQ(gamepadButtonFromKeycode(21), GamepadButton::DpadLeft);
    EXPECT_EQ(gamepadButtonFromKeycode(29), std::nullopt);
}

TEST(AndroidKeymap, GamepadAxesFlipYAndPickRightStickAxes) {
    using namespace android_input;
    std::array<f32, kAxisCount> v{};
    v[kAxisX] = 0.5f;
    v[kAxisY] = -1.0f; // Android: up is negative
    v[kAxisZ] = 0.25f;
    v[kAxisRZ] = 0.75f;
    v[kAxisBrake] = 0.6f;
    v[kAxisRTrigger] = 1.5f;
    v[kAxisHatX] = -1.0f;
    GamepadSample s = gamepadFromAxes(v);
    EXPECT_FLOAT_EQ(s.axes[usize(GamepadAxis::LeftX)], 0.5f);
    EXPECT_FLOAT_EQ(s.axes[usize(GamepadAxis::LeftY)], 1.0f);
    EXPECT_FLOAT_EQ(s.axes[usize(GamepadAxis::RightX)], 0.25f);
    EXPECT_FLOAT_EQ(s.axes[usize(GamepadAxis::RightY)], -0.75f);
    EXPECT_FLOAT_EQ(s.axes[usize(GamepadAxis::LeftTrigger)], 0.6f);
    EXPECT_FLOAT_EQ(s.axes[usize(GamepadAxis::RightTrigger)], 1.0f);
    EXPECT_TRUE(s.dpadLeft);
    EXPECT_FALSE(s.dpadRight || s.dpadUp || s.dpadDown);

    std::array<f32, kAxisCount> rx{};
    rx[kAxisRX] = -0.5f;
    rx[kAxisRY] = 0.5f;
    s = gamepadFromAxes(rx);
    EXPECT_FLOAT_EQ(s.axes[usize(GamepadAxis::RightX)], -0.5f) << "RX/RY when Z/RZ are idle";
    EXPECT_FLOAT_EQ(s.axes[usize(GamepadAxis::RightY)], -0.5f);
    EXPECT_FLOAT_EQ(gamepadFromAxes(std::span<const f32>{}).axes[0], 0.0f);
}
