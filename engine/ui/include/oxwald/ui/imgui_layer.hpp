#pragma once

// ImGuiLayer: owns a Dear ImGui context (docking), feeds it engine input events, rasterises fonts into the shared
// UiTextureStore (ImGui 1.92 dynamic textures) and converts ImDrawData into a UiFrame for UiOverlayFeature.
// No ImGui platform/renderer backends are used: input comes from ox::InputEvent, rendering is rhi.
//
//   ImGuiLayer imgui(bridge.textures());
//   imgui.beginFrame(dt, framebufferSize, dpiScale);   // game thread; makes the context current
//   ImGui::Begin("Hello"); ImGui::Text("Привет"); ImGui::End();
//   imgui.endFrame(frame);                             // ImGui::Render + texture requests + draw data -> frame
//
// The debug overlay (menu bar, dock space, tool windows registered with addWindow) is shown while visible()
// (toggle with F1 or `~`); windows submitted directly (scripts, game code) are always drawn.

#include <oxwald/core/events.hpp>
#include <oxwald/runtime/input.hpp>
#include <oxwald/ui/draw_data.hpp>

#include <glm/vec2.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

struct ImGuiContext;
struct ImFont;

namespace ox::ui {

struct ImGuiLayerConfig {
    f32 fontSize = 15.0f;  // logical pixels (multiplied by the DPI scale)
    bool docking = true;
    std::string iniPath;   // window layout persistence; empty = none
    bool visible = false;  // debug overlay initially shown
    std::vector<Key> toggleKeys{Key::F1, Key::GraveAccent};
};

class ImGuiLayer {
public:
    using WindowFn = std::function<void(bool& open)>;

    explicit ImGuiLayer(UiTextureStore& textures, ImGuiLayerConfig config = {});
    ~ImGuiLayer();
    ImGuiLayer(const ImGuiLayer&) = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;

    [[nodiscard]] ImGuiContext* context() const { return m_context; }
    void makeCurrent() const;

    // ---- frame (game thread) ----
    // framebufferSize in pixels; ImGui works in logical points = pixels / dpiScale.
    void beginFrame(f64 dt, glm::uvec2 framebufferSize, f32 dpiScale = 1.0f);
    // Ends the frame (ImGui::Render), applies texture requests and appends the draw data to `out` (size = framebuffer).
    void endFrame(UiFrame& out);
    [[nodiscard]] bool inFrame() const { return m_inFrame; }

    // ---- input ----
    // `pixelPosition` events: mouse positions must already be framebuffer pixels (UiInputRouter converts).
    // Returns true when ImGui consumes the event (the game must not see it). Releases are never consumed.
    bool processEvent(const InputEvent& event);
    [[nodiscard]] bool wantsMouse() const;
    [[nodiscard]] bool wantsKeyboard() const;
    [[nodiscard]] bool wantsTextInput() const;

    // ---- debug overlay ----
    [[nodiscard]] bool visible() const { return m_visible; }
    void setVisible(bool visible) { m_visible = visible; }
    void toggle() { m_visible = !m_visible; }
    // Tool window shown from the overlay's "Windows" menu (and drawn while visible + open). Returns an id.
    u32 addWindow(std::string menuPath, WindowFn fn, bool open = false);
    void removeWindow(u32 id);
    void setWindowOpen(std::string_view menuPath, bool open);
    [[nodiscard]] bool windowOpen(std::string_view menuPath) const;
    // Drawn every frame regardless of visibility (e.g. the stats HUD toggled by a cvar).
    u32 addAlwaysOnTop(std::function<void()> fn);
    // Extra items in the overlay's main menu bar.
    u32 addMenu(std::function<void()> fn);

    // ---- textures / fonts ----
    // ImTextureID for a bindless sampled-image index (render targets, asset textures): ImGui::Image(ref, size).
    [[nodiscard]] static u64 textureId(u32 bindlessIndex) { return bindlessTexture(bindlessIndex); }
    [[nodiscard]] ImFont* uiFont() const { return m_uiFont; }
    [[nodiscard]] ImFont* monoFont() const { return m_monoFont; }
    [[nodiscard]] f32 dpiScale() const { return m_dpiScale; }

    Signal<> frameStarted; // after NewFrame (overlay not drawn yet)

private:
    struct Window {
        u32 id = 0;
        std::string path;
        WindowFn fn;
        bool open = false;
    };
    void applyTextures();
    void destroyTextures();
    void drawOverlay();
    void setupStyle();

    UiTextureStore& m_textures;
    ImGuiLayerConfig m_config;
    ImGuiContext* m_context = nullptr;
    ImFont* m_uiFont = nullptr;
    ImFont* m_monoFont = nullptr;
    std::vector<std::vector<u8>> m_fontData; // kept alive for the atlas (ImGui 1.92 rasterises on demand)
    bool m_inFrame = false;
    bool m_visible = false;
    bool m_swallowText = false;
    f32 m_dpiScale = 1.0f;
    glm::uvec2 m_framebuffer{0, 0};
    std::vector<Window> m_windows;
    std::vector<std::pair<u32, std::function<void()>>> m_alwaysOnTop;
    std::vector<std::pair<u32, std::function<void()>>> m_menus;
    u32 m_nextId = 1;
    bool m_ctrl[2]{}, m_shift[2]{}, m_alt[2]{}, m_super[2]{};
};

// ox::Key -> ImGuiKey (ImGuiKey_None when unmapped).
[[nodiscard]] int toImGuiKey(Key key);

} // namespace ox::ui
