#pragma once

// In-game debug tools drawn with ImGui (registered as overlay windows of an ImGuiLayer):
//   Stats            FPS, frame-time graph, game/render thread CPU, GPU per pass, draw calls, triangles, VRAM, culling
//   Console          runtime Console backend: log view with level/text filters, history, Tab completion
//   Settings & CVars quality presets Low/Medium/High/Ultra, per-group levels, RT (greyed + reason), upscaler with
//                    availability reasons, every cvar editable (filter by name)
//   Entity Inspector hierarchy of the active world, reflection-driven component editing
//   Render Graph     passes (queue, batch, barriers, GPU ms, culled), resources (alias slots, sizes), Graphviz export
//   Coroutines       CoroutineScheduler introspection (async module)
//   Debug Draw       physics / AI / animation / audio / spline debug drawing toggles (gameplay module), debug views
// The stats HUD (ui.ShowStats 1) is also drawn while the overlay is hidden.

#include <oxwald/core/types.hpp>

#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ox {
class Engine;
class Console;
class Services;
class Settings;
class World;
} // namespace ox

namespace ox::ui {

class ImGuiLayer;
class UiRenderBridge;
class GameUI;

struct DebugToolsContext {
    Engine* engine = nullptr;     // stats, active world, services, console, settings (when set)
    Console* console = nullptr;   // defaults to engine->console()
    Services* services = nullptr; // defaults to engine->services()
    Settings* settings = nullptr; // defaults to engine->settings()
    World* world = nullptr;       // inspector target; defaults to engine->world()
    UiRenderBridge* bridge = nullptr;
    GameUI* gameUI = nullptr;
};

class DebugTools {
public:
    static constexpr std::string_view kStats = "Stats";
    static constexpr std::string_view kConsole = "Console";
    static constexpr std::string_view kSettings = "Settings & CVars";
    static constexpr std::string_view kInspector = "Entity Inspector";
    static constexpr std::string_view kRenderGraph = "Render Graph";
    static constexpr std::string_view kCoroutines = "Coroutines";
    static constexpr std::string_view kDebugDraw = "Debug Draw";

    DebugTools(ImGuiLayer& imgui, DebugToolsContext context);
    ~DebugTools();
    DebugTools(const DebugTools&) = delete;
    DebugTools& operator=(const DebugTools&) = delete;

    // Once per frame (before the windows draw): frame-time history etc.
    void tick(f64 realDt);
    void open(std::string_view window, bool open = true);

    // Console input as typed into the console window (history + output).
    void consoleSubmit(std::string_view line);
    [[nodiscard]] Console* console() const;

    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace ox::ui
