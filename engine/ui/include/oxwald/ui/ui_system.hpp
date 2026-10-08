#pragma once

// UiSystem: the ui module's service. Owns the render bridge, the ImGui layer + debug tools and the RmlUi game UI,
// routes input (ImGui overlay first, then the game UI, then the game) and publishes one UiFrame per frame.
//
// Engine integration (UiModule, see ui_module.hpp) does all of this automatically; standalone use (tests, tools):
//
//   ox::ui::UiSystem ui({}, &vfs);
//   ox::ui::attachRenderer(renderer, ui.bridgeShared());
//   loop: ui.processEvent(e)...;  ui.beginFrame(dt, fb, dpi);  /* ImGui calls */  ui.endFrame();  render

#include <oxwald/core/events.hpp>
#include <oxwald/runtime/input.hpp>
#include <oxwald/ui/debug_tools.hpp>
#include <oxwald/ui/draw_data.hpp>
#include <oxwald/ui/game_ui.hpp>
#include <oxwald/ui/imgui_layer.hpp>

#include <glm/vec2.hpp>

#include <memory>

namespace ox {
class Engine;
class Vfs;
namespace script {
class ScriptVM;
}
} // namespace ox

namespace ox::ui {

struct UiConfig {
    bool imgui = true;
    bool gameUI = true;
    bool debugTools = true;       // engine attach: register the debug windows
    bool bindSettingsMenu = true; // engine attach: data model "settings" over Engine::settings()
    ImGuiLayerConfig imguiConfig;
    GameUIConfig gameUIConfig;
    glm::uvec2 headlessSize{1280, 720}; // UI size without a platform window
    // Platform cursor coordinates -> framebuffer pixels. 0 = auto: the DPI scale on macOS (GLFW reports points),
    // 1 elsewhere.
    f32 cursorScale = 0.0f;
};

class UiSystem {
public:
    explicit UiSystem(UiConfig config = {}, Vfs* vfs = nullptr);
    ~UiSystem();
    UiSystem(const UiSystem&) = delete;
    UiSystem& operator=(const UiSystem&) = delete;

    // ---- engine wiring (UiModule) ----
    // Input filter on engine.input(), debug tools, settings menu model, console commands, Lua APIs.
    void attach(Engine& engine);
    void detach();
    [[nodiscard]] Engine* engine() const { return m_engine; }
    // Debug windows without an engine (tests/tools). attach() creates them from the engine.
    DebugTools& enableDebugTools(DebugToolsContext context);
#if OX_UI_HAS_SCRIPT
    // Lua `ui` (game UI) and `debug` (immediate-mode ImGui) tables. The VM must outlive the bindings: call
    // unbindLua() before destroying it (the engine destroys the UiSystem service before the ScriptVM).
    void bindLua(script::ScriptVM& vm);
    void unbindLua();
#endif

    // ---- frame (game thread) ----
    void beginFrame(f64 dt, glm::uvec2 framebufferSize, f32 dpiScale = 1.0f);
    void endFrame();
    [[nodiscard]] bool inFrame() const { return m_inFrame; }
    [[nodiscard]] std::shared_ptr<const UiFrame> lastFrame() const { return m_lastFrame; }
    [[nodiscard]] glm::uvec2 framebufferSize() const { return m_framebuffer; }

    // ---- input: true = consumed by the UI (the game must not see it) ----
    bool processEvent(const InputEvent& event);
    [[nodiscard]] f32 cursorScale() const;

    // Emitted on the game thread after an "Auto" quality benchmark result was applied (summary text).
    Signal<const std::string&> qualityAutoDetected;

    // ---- parts ----
    [[nodiscard]] ImGuiLayer* imgui() const { return m_imgui.get(); }
    [[nodiscard]] GameUI* gameUI() const { return m_gameUI.get(); }
    [[nodiscard]] DebugTools* debugTools() const { return m_debugTools.get(); }
    [[nodiscard]] UiRenderBridge& bridge() { return *m_bridge; }
    [[nodiscard]] const std::shared_ptr<UiRenderBridge>& bridgeShared() const { return m_bridge; }
    [[nodiscard]] const UiConfig& config() const { return m_config; }

    struct LuaState; // Lua binding state (ui_lua.cpp)

private:
    void handleBenchmark();
    void applyOverlayCursor();

    UiConfig m_config;
    Vfs* m_vfs;
    std::shared_ptr<UiRenderBridge> m_bridge;
    std::unique_ptr<ImGuiLayer> m_imgui;
    std::unique_ptr<GameUI> m_gameUI;
    std::unique_ptr<DebugTools> m_debugTools;
    std::unique_ptr<LuaState> m_lua;
    std::shared_ptr<const UiFrame> m_lastFrame;
    Engine* m_engine = nullptr;
    bool m_inFrame = false;
    glm::uvec2 m_framebuffer{0, 0};
    f32 m_dpi = 1.0f;
    u64 m_serial = 0;
    bool m_overlayWasVisible = false;
    int m_savedCursor = -1;
    std::vector<ScopedConnection> m_connections;

};

} // namespace ox::ui
