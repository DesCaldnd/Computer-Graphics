#pragma once

#include <oxwald/core/result.hpp>
#include <oxwald/runtime/input.hpp>
#include <oxwald/runtime/renderer.hpp>
#include <oxwald/runtime/settings.hpp>

#include <glm/vec2.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct GLFWwindow;
struct GLFWmonitor;

namespace ox {

// Window/event source driven by Engine::run(). Implemented by GlfwPlatform (player) and the Qt editor viewport.
class IPlatform {
public:
    virtual ~IPlatform() = default;
    // Pumps OS events and injects input into the InputSystem. Game (main) thread.
    virtual void pollEvents() = 0;
    [[nodiscard]] virtual bool shouldClose() const = 0;
    [[nodiscard]] virtual RenderSurface surface() const = 0;
    [[nodiscard]] virtual glm::uvec2 framebufferSize() const = 0;
    // Applies window mode / resolution / monitor at runtime.
    virtual void applyWindowSettings(const GraphicsSettings&) {}
    virtual void setCursorMode(CursorMode) {}
};

struct VideoMode {
    glm::ivec2 size{0, 0};
    i32 refreshRate = 0;
    friend bool operator==(const VideoMode&, const VideoMode&) = default;
};

struct MonitorInfo {
    std::string name;
    glm::ivec2 position{0, 0};
    VideoMode current;
    std::vector<VideoMode> modes;
    glm::vec2 contentScale{1.0f, 1.0f};
};

struct WindowDesc {
    std::string title = "Oxwald";
    glm::ivec2 size{1600, 900}; // windowed size; fullscreen uses GraphicsSettings::resolution or the desktop mode
    WindowMode mode = WindowMode::Windowed;
    i32 monitor = 0;
    bool resizable = true;
    bool visible = true;
};

#if OX_HAS_GLFW
// GLFW player platform: window for Vulkan (GLFW_NO_API), input events -> InputSystem, gamepads (GLFW gamepad
// mappings), window modes, monitors, DPI, cursor modes, clipboard. All calls on the main thread.
class GlfwPlatform final : public IPlatform {
public:
    // Initialises GLFW (once per process; through rhi::initGlfwVulkan when rhi is linked) and opens the window.
    [[nodiscard]] static Result<std::unique_ptr<GlfwPlatform>> create(const WindowDesc& desc, InputSystem* input);
    ~GlfwPlatform() override;
    GlfwPlatform(const GlfwPlatform&) = delete;
    GlfwPlatform& operator=(const GlfwPlatform&) = delete;

    void pollEvents() override;
    [[nodiscard]] bool shouldClose() const override;
    void requestClose();
    [[nodiscard]] RenderSurface surface() const override;
    [[nodiscard]] glm::uvec2 framebufferSize() const override;
    void applyWindowSettings(const GraphicsSettings& settings) override;
    void setCursorMode(CursorMode mode) override;

    void setWindowMode(WindowMode mode, glm::ivec2 resolution = {0, 0}, i32 monitor = 0);
    [[nodiscard]] WindowMode windowMode() const { return m_mode; }
    [[nodiscard]] std::vector<MonitorInfo> monitors() const;
    [[nodiscard]] glm::vec2 contentScale() const;
    void setTitle(const std::string& title);
    [[nodiscard]] std::string clipboard() const;
    void setClipboard(const std::string& text);
    [[nodiscard]] GLFWwindow* window() const { return m_window; }
    void setInput(InputSystem* input);
    // Gamepad mapping database (SDL_GameControllerDB format) for pads GLFW does not know.
    static bool updateGamepadMappings(const std::string& mappings);

    // GLFW key code -> engine Key (exposed for tests and the editor).
    [[nodiscard]] static Key translateKey(int glfwKey);

private:
    GlfwPlatform() = default;
    void pollGamepads();
    static void installCallbacks(GLFWwindow* window);

    GLFWwindow* m_window = nullptr;
    InputSystem* m_input = nullptr;
    WindowMode m_mode = WindowMode::Windowed;
    glm::ivec2 m_windowedPos{100, 100};
    glm::ivec2 m_windowedSize{1600, 900};
    glm::dvec2 m_lastCursor{0.0, 0.0};
    bool m_hasCursor = false;
    struct PadState {
        bool connected = false;
        std::array<bool, kGamepadButtonCount> buttons{};
        std::array<f32, kGamepadAxisCount> axes{};
    };
    std::array<PadState, kMaxGamepads> m_pads{};
    std::unique_ptr<void, void (*)(void*)> m_surfaceProvider{nullptr, nullptr}; // rhi::GlfwSurfaceProvider
    ScopedConnection m_cursorConnection;
};
#endif

} // namespace ox
