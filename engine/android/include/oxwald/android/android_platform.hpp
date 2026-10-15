#pragma once

// ox::IPlatform for Android (GameActivity native_app_glue). Owns the Vulkan surface provider, feeds the
// AppLifecycle state machine from APP_CMD_* commands and turns GameActivity input buffers into InputEvents:
//   touch screen -> TouchInputMapper (virtual stick / look / tap / buttons / pinch)
//   gamepads     -> GamepadButton / GamepadAxis (slots 0..2, by device id; slot 3 is the touch pad)
//   keyboards    -> Key + Text (hardware keyboards, the emulator's host keyboard; BACK = Escape)
//
// Threading: everything runs on the android_main thread (the engine's game thread). pumpCommands() must be called
// outside Engine::tick() (it may tick the engine itself to let the render thread drop a dying window's
// swapchain); pollEvents(), called by the engine every frame, only drains input.
#include <oxwald/rhi/android_surface.hpp>
#include <oxwald/runtime/app_lifecycle.hpp>
#include <oxwald/runtime/platform.hpp>
#include <oxwald/runtime/touch_input.hpp>

#include <array>
#include <functional>
#include <map>
#include <vector>

struct android_app;

namespace ox {
class Engine;
}

namespace ox::android {

class AndroidPlatform final : public IPlatform {
public:
    explicit AndroidPlatform(android_app* app);
    ~AndroidPlatform() override;
    AndroidPlatform(const AndroidPlatform&) = delete;
    AndroidPlatform& operator=(const AndroidPlatform&) = delete;

    // After Engine::init: input target, audio/settings hooks, ticking during window loss.
    void attach(Engine* engine);
    void detach();

    // Processes looper commands (lifecycle). block = wait for the next event (nothing to render).
    void pumpCommands(bool block);

    // IPlatform
    void pollEvents() override; // input only
    [[nodiscard]] bool shouldClose() const override;
    [[nodiscard]] RenderSurface surface() const override;
    [[nodiscard]] glm::uvec2 framebufferSize() const override;

    [[nodiscard]] const AppLifecycle& lifecycle() const { return m_lifecycle; }
    [[nodiscard]] rhi::AndroidSurfaceProvider& surfaceProvider() { return m_provider; }
    [[nodiscard]] TouchInputMapper& touch() { return m_touch; }
    [[nodiscard]] android_app* app() const { return m_app; }
    [[nodiscard]] f32 dpiScale() const;
    // GameActivity_finish: the activity closes, APP_CMD_DESTROY follows.
    void finish();

    // Optional hook for the app (e.g. on-screen touch overlay, logging); called after every lifecycle event.
    std::function<void(AppEvent, const LifecycleActions&)> onLifecycle;

private:
    static void onAppCmd(android_app* app, int32_t cmd);
    void handleCommand(int32_t cmd);
    void apply(AppEvent event, const LifecycleActions& actions);
    void releaseWindow();
    void processMotion(const void* event);
    void processKey(const void* event);
    u32 padSlot(i32 deviceId);
    void inject(const InputEvent& e);

    android_app* m_app = nullptr;
    Engine* m_engine = nullptr;
    AppLifecycle m_lifecycle;
    rhi::AndroidSurfaceProvider m_provider;
    TouchInputMapper m_touch;
    std::vector<InputEvent> m_events;
    std::map<i32, u32> m_padSlots; // input device id -> gamepad slot
    struct PadState {
        std::array<f32, kGamepadAxisCount> axes{};
        bool hat[4]{};
    };
    std::array<PadState, kMaxGamepads> m_pads{};
    bool m_quit = false;
};

} // namespace ox::android
