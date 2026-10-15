#include <oxwald/android/android_platform.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/runtime/android_keymap.hpp>
#include <oxwald/runtime/engine.hpp>
#if OX_ANDROID_HAS_AUDIO
#include <oxwald/audio/audio_engine.hpp>
#endif

#include <android/configuration.h>
#include <android/input.h>
#include <android/looper.h>
#include <android/native_window.h>
#include <game-activity/GameActivity.h>
#include <game-activity/native_app_glue/android_native_app_glue.h>

#include <chrono>
#include <cmath>

namespace ox::android {

namespace {

constexpr u32 kTouchPadSlot = kMaxGamepads - 1;

const char* commandName(int32_t cmd) {
    switch (cmd) {
    case APP_CMD_INIT_WINDOW: return "INIT_WINDOW";
    case APP_CMD_TERM_WINDOW: return "TERM_WINDOW";
    case APP_CMD_WINDOW_RESIZED: return "WINDOW_RESIZED";
    case APP_CMD_WINDOW_REDRAW_NEEDED: return "WINDOW_REDRAW_NEEDED";
    case APP_CMD_CONTENT_RECT_CHANGED: return "CONTENT_RECT_CHANGED";
    case APP_CMD_GAINED_FOCUS: return "GAINED_FOCUS";
    case APP_CMD_LOST_FOCUS: return "LOST_FOCUS";
    case APP_CMD_CONFIG_CHANGED: return "CONFIG_CHANGED";
    case APP_CMD_LOW_MEMORY: return "LOW_MEMORY";
    case APP_CMD_START: return "START";
    case APP_CMD_RESUME: return "RESUME";
    case APP_CMD_SAVE_STATE: return "SAVE_STATE";
    case APP_CMD_PAUSE: return "PAUSE";
    case APP_CMD_STOP: return "STOP";
    case APP_CMD_DESTROY: return "DESTROY";
    case APP_CMD_WINDOW_INSETS_CHANGED: return "WINDOW_INSETS_CHANGED";
    default: return "?";
    }
}

bool isGamepadSource(int32_t source) {
    return (source & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD ||
           (source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK;
}

} // namespace

AndroidPlatform::AndroidPlatform(android_app* app) : m_app(app) {
    m_app->userData = this;
    m_app->onAppCmd = &AndroidPlatform::onAppCmd;
    // Accept every input source (the default filters only pass touch screens / keyboards).
    android_app_set_motion_event_filter(m_app, nullptr);
    android_app_set_key_event_filter(m_app, nullptr);
    // GameActivity only copies X/Y per pointer unless more axes are enabled.
    for (int32_t axis : {AMOTION_EVENT_AXIS_Z, AMOTION_EVENT_AXIS_RX, AMOTION_EVENT_AXIS_RY, AMOTION_EVENT_AXIS_RZ,
                         AMOTION_EVENT_AXIS_HAT_X, AMOTION_EVENT_AXIS_HAT_Y, AMOTION_EVENT_AXIS_LTRIGGER,
                         AMOTION_EVENT_AXIS_RTRIGGER, AMOTION_EVENT_AXIS_GAS, AMOTION_EVENT_AXIS_BRAKE}) {
        GameActivityPointerAxes_enableAxis(axis);
    }
    TouchLayout layout = TouchLayout::defaults();
    layout.gamepadSlot = kTouchPadSlot;
    m_touch.setLayout(layout);
}

AndroidPlatform::~AndroidPlatform() {
    if (m_app) {
        m_app->onAppCmd = nullptr;
        m_app->userData = nullptr;
    }
}

void AndroidPlatform::attach(Engine* engine) { m_engine = engine; }
void AndroidPlatform::detach() { m_engine = nullptr; }

void AndroidPlatform::onAppCmd(android_app* app, int32_t cmd) {
    if (auto* self = static_cast<AndroidPlatform*>(app->userData)) self->handleCommand(cmd);
}

void AndroidPlatform::pumpCommands(bool block) {
    for (;;) {
        android_poll_source* source = nullptr;
        int events = 0;
        const int timeout = block && !m_app->destroyRequested && !m_lifecycle.shouldRender() ? -1 : 0;
        const int r = ALooper_pollOnce(timeout, nullptr, &events, reinterpret_cast<void**>(&source));
        if (r == ALOOPER_POLL_ERROR) break;
        if (source) source->process(m_app, source);
        if (r == ALOOPER_POLL_TIMEOUT || m_app->destroyRequested) break;
        if (block && m_lifecycle.shouldRender()) block = false; // drain the rest without waiting
    }
}

void AndroidPlatform::handleCommand(int32_t cmd) {
    OX_LOG_DEBUG("android", "APP_CMD_{}", commandName(cmd));
    auto fire = [&](AppEvent e) { apply(e, m_lifecycle.handle(e)); };
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        if (m_app->window) {
            m_provider.setWindow(m_app->window);
            m_touch.setViewport(glm::vec2(framebufferSize()));
            OX_LOG_INFO("android", "window {}x{} (density {:.2f})", ANativeWindow_getWidth(m_app->window),
                        ANativeWindow_getHeight(m_app->window), dpiScale());
            fire(AppEvent::WindowCreated);
        }
        break;
    case APP_CMD_TERM_WINDOW:
        fire(AppEvent::WindowDestroyed);
        break;
    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONTENT_RECT_CHANGED:
        m_touch.setViewport(glm::vec2(framebufferSize()));
        fire(AppEvent::WindowResized);
        break;
    case APP_CMD_CONFIG_CHANGED:
        m_touch.setViewport(glm::vec2(framebufferSize()));
        fire(AppEvent::ConfigChanged);
        break;
    case APP_CMD_GAINED_FOCUS: fire(AppEvent::FocusGained); break;
    case APP_CMD_LOST_FOCUS: fire(AppEvent::FocusLost); break;
    case APP_CMD_START: fire(AppEvent::Start); break;
    case APP_CMD_RESUME: fire(AppEvent::Resume); break;
    case APP_CMD_PAUSE: fire(AppEvent::Pause); break;
    case APP_CMD_STOP: fire(AppEvent::Stop); break;
    case APP_CMD_SAVE_STATE: fire(AppEvent::SaveState); break;
    case APP_CMD_LOW_MEMORY: fire(AppEvent::LowMemory); break;
    case APP_CMD_DESTROY: fire(AppEvent::Destroy); break;
    default: break;
    }
}

void AndroidPlatform::releaseWindow() {
    ANativeWindow* old = m_provider.window();
    m_provider.setWindow(nullptr); // surfaceGeneration changes; framebufferSize() is 0x0 from now on
    if (!old) return;
    // The Java side waits for this command to finish before it destroys the Surface: let the render thread drop
    // the swapchain + VkSurfaceKHR first (it does so in the next Swapchain::acquire). Bounded: a stuck renderer
    // must not ANR the activity (the provider keeps the window object alive until Vulkan released it anyway).
    bool released = m_provider.waitForRelease(old, std::chrono::milliseconds(0));
    for (int i = 0; i < 3 && !released && m_engine && m_engine->initialized(); ++i) {
        m_engine->tick();
        released = m_provider.waitForRelease(old, std::chrono::milliseconds(100));
    }
    OX_LOG_INFO("android", "window released by the renderer: {}", released ? "yes" : "pending (timeout)");
}

void AndroidPlatform::apply(AppEvent event, const LifecycleActions& a) {
    if (a.any()) OX_LOG_INFO("android", "lifecycle {} -> {} ({})", appEventName(event), a.toString(), m_lifecycle.describe());
    if (a.destroySurface && event != AppEvent::WindowCreated) releaseWindow();
    if (a.releaseInput) {
        m_events.clear();
        m_touch.reset(m_events);
        for (const InputEvent& e : m_events) inject(e);
        m_events.clear();
        inject(InputEvent::focusLost());
    }
#if OX_ANDROID_HAS_AUDIO
    if ((a.pauseAudio || a.resumeAudio) && m_engine && m_engine->initialized()) {
        if (auto* audio = m_engine->services().tryGet<audio::AudioEngine>(); audio && audio->initialized()) {
            audio->setDeviceSuspended(a.pauseAudio);
        }
    }
#endif
    if (a.saveState && m_engine && m_engine->initialized()) {
        if (auto st = m_engine->settings().save(); !st) OX_LOG_WARN("android", "saving settings: {}", st.error().message);
    }
    if (a.trimMemory) OX_LOG_WARN("android", "low memory warning from the system");
    if (a.quit) m_quit = true;
    if (onLifecycle) onLifecycle(event, a);
}

bool AndroidPlatform::shouldClose() const { return m_quit || m_app->destroyRequested; }

RenderSurface AndroidPlatform::surface() const {
    RenderSurface s;
    s.provider = const_cast<rhi::AndroidSurfaceProvider*>(&m_provider);
    s.nativeWindow = m_app->window;
    s.framebufferSize = framebufferSize();
    s.dpiScale = dpiScale();
    return s;
}

glm::uvec2 AndroidPlatform::framebufferSize() const {
    const VkExtent2D e = m_provider.framebufferSize();
    return {e.width, e.height};
}

f32 AndroidPlatform::dpiScale() const {
    const int32_t density = m_app->config ? AConfiguration_getDensity(m_app->config) : 0;
    return density > 0 ? f32(density) / 160.0f : 1.0f;
}

void AndroidPlatform::finish() { GameActivity_finish(m_app->activity); }

void AndroidPlatform::inject(const InputEvent& e) {
    if (m_engine && m_engine->initialized()) m_engine->input().inject(e);
}

u32 AndroidPlatform::padSlot(i32 deviceId) {
    if (auto it = m_padSlots.find(deviceId); it != m_padSlots.end()) return it->second;
    const u32 slot = u32(m_padSlots.size()) < kTouchPadSlot ? u32(m_padSlots.size()) : kTouchPadSlot - 1;
    m_padSlots[deviceId] = slot;
    inject(InputEvent::gamepadConnected(slot, true));
    OX_LOG_INFO("android", "gamepad (device {}) -> slot {}", deviceId, slot);
    return slot;
}

void AndroidPlatform::processMotion(const void* ev) {
    const auto& me = *static_cast<const GameActivityMotionEvent*>(ev);
    if (isGamepadSource(me.source) && me.pointerCount > 0) {
        std::array<f32, android_input::kAxisCount> values{};
        for (i32 axis = 0; axis < android_input::kAxisCount; ++axis) {
            values[usize(axis)] = GameActivityPointerAxes_getAxisValue(&me.pointers[0], axis);
        }
        const u32 slot = padSlot(me.deviceId);
        const android_input::GamepadSample s = android_input::gamepadFromAxes(values);
        PadState& pad = m_pads[slot];
        for (usize a = 0; a < kGamepadAxisCount; ++a) {
            if (std::abs(s.axes[a] - pad.axes[a]) > 1e-4f) {
                pad.axes[a] = s.axes[a];
                inject(InputEvent::gamepadAxis(slot, GamepadAxis(a), s.axes[a]));
            }
        }
        const bool hat[4] = {s.dpadUp, s.dpadRight, s.dpadDown, s.dpadLeft};
        constexpr GamepadButton kHat[4] = {GamepadButton::DpadUp, GamepadButton::DpadRight, GamepadButton::DpadDown,
                                           GamepadButton::DpadLeft};
        for (int i = 0; i < 4; ++i) {
            if (hat[i] != pad.hat[i]) {
                pad.hat[i] = hat[i];
                inject(InputEvent::gamepadButton(slot, kHat[i], hat[i]));
            }
        }
        return;
    }
    // Touch screen (and the emulator's mouse, which arrives as a finger).
    const int32_t action = me.action & AMOTION_EVENT_ACTION_MASK;
    const int32_t index = (me.action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    // Tap detection only needs a monotonic clock; eventTime's unit differs between GameActivity versions.
    const f64 time = std::chrono::duration<f64>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto point = [&](int32_t i) {
        return glm::vec2(GameActivityPointerAxes_getX(&me.pointers[i]), GameActivityPointerAxes_getY(&me.pointers[i]));
    };
    m_events.clear();
    switch (action) {
    case AMOTION_EVENT_ACTION_DOWN:
    case AMOTION_EVENT_ACTION_POINTER_DOWN:
        if (index < i32(me.pointerCount))
            m_touch.process({TouchPhase::Down, me.pointers[index].id, point(index), time}, m_events);
        break;
    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_POINTER_UP:
        if (index < i32(me.pointerCount))
            m_touch.process({TouchPhase::Up, me.pointers[index].id, point(index), time}, m_events);
        break;
    case AMOTION_EVENT_ACTION_MOVE:
        for (int32_t i = 0; i < i32(me.pointerCount); ++i)
            m_touch.process({TouchPhase::Move, me.pointers[i].id, point(i), time}, m_events);
        break;
    case AMOTION_EVENT_ACTION_CANCEL:
        for (int32_t i = 0; i < i32(me.pointerCount); ++i)
            m_touch.process({TouchPhase::Cancel, me.pointers[i].id, point(i), time}, m_events);
        break;
    default: break;
    }
    for (const InputEvent& e : m_events) inject(e);
}

void AndroidPlatform::processKey(const void* ev) {
    const auto& ke = *static_cast<const GameActivityKeyEvent*>(ev);
    if (ke.action != AKEY_EVENT_ACTION_DOWN && ke.action != AKEY_EVENT_ACTION_UP) return;
    const bool down = ke.action == AKEY_EVENT_ACTION_DOWN;
    if (isGamepadSource(ke.source) || (ke.keyCode >= AKEYCODE_BUTTON_A && ke.keyCode <= AKEYCODE_BUTTON_MODE)) {
        if (auto b = android_input::gamepadButtonFromKeycode(ke.keyCode)) {
            if (ke.repeatCount == 0) inject(InputEvent::gamepadButton(padSlot(ke.deviceId), *b, down));
            return;
        }
    }
    if (auto k = android_input::keyFromKeycode(ke.keyCode)) inject(InputEvent::key(*k, down, ke.repeatCount > 0));
    if (down && ke.unicodeChar >= 0x20) inject(InputEvent::text(u32(ke.unicodeChar)));
}

void AndroidPlatform::pollEvents() {
    android_input_buffer* buffer = android_app_swap_input_buffers(m_app);
    if (!buffer) return;
    for (u64 i = 0; i < buffer->motionEventsCount; ++i) processMotion(&buffer->motionEvents[i]);
    for (u64 i = 0; i < buffer->keyEventsCount; ++i) processKey(&buffer->keyEvents[i]);
    android_app_clear_motion_events(buffer);
    android_app_clear_key_events(buffer);
}

} // namespace ox::android
