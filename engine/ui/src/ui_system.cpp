#include <oxwald/ui/ui_system.hpp>

#include "lua_state.hpp"

#include <oxwald/core/log.hpp>
#include <oxwald/core/profile.hpp>
#include <oxwald/core/scalability.hpp>
#include <oxwald/render/quality.hpp>
#include <oxwald/runtime/engine.hpp>
#include <oxwald/runtime/platform.hpp>
#include <oxwald/runtime/settings.hpp>

#if OX_UI_HAS_SCRIPT
#include <oxwald/script/script_vm.hpp>
#endif

#include <imgui.h>

#include <algorithm>
#include <array>
#include <set>

namespace ox::ui {

#if OX_UI_HAS_SCRIPT
void bindUiLua(script::ScriptVM& vm, UiSystem& ui, std::unique_ptr<UiSystem::LuaState>& state);
void closeDanglingLuaWindows(UiSystem::LuaState* state);
void destroyLuaState(std::unique_ptr<UiSystem::LuaState>& state);
#endif

UiSystem::UiSystem(UiConfig config, Vfs* vfs) : m_config(std::move(config)), m_vfs(vfs), m_bridge(std::make_shared<UiRenderBridge>()) {
    if (m_config.imgui) m_imgui = std::make_unique<ImGuiLayer>(m_bridge->textures(), m_config.imguiConfig);
    if (m_config.gameUI) m_gameUI = std::make_unique<GameUI>(m_bridge->textures(), vfs, m_config.gameUIConfig);
    m_framebuffer = m_config.headlessSize;
}

UiSystem::~UiSystem() {
    detach();
#if OX_UI_HAS_SCRIPT
    destroyLuaState(m_lua);
#endif
    m_debugTools.reset();
    if (m_imgui && m_inFrame) {
        m_imgui->makeCurrent();
        ImGui::EndFrame();
    }
    m_gameUI.reset();
    m_imgui.reset();
}

DebugTools& UiSystem::enableDebugTools(DebugToolsContext context) {
    OX_ASSERT(m_imgui, "debug tools need the ImGui layer");
    m_debugTools.reset();
    if (!context.bridge) context.bridge = m_bridge.get();
    if (!context.gameUI) context.gameUI = m_gameUI.get();
    m_debugTools = std::make_unique<DebugTools>(*m_imgui, context);
    return *m_debugTools;
}

void UiSystem::attach(Engine& engine) {
    detach();
    m_engine = &engine;
    engine.input().setEventFilter([this](const InputEvent& e) { return processEvent(e); });
    if (m_imgui && m_config.debugTools) enableDebugTools({.engine = &engine});

    if (m_gameUI && m_config.bindSettingsMenu) {
        SettingsMenuHooks hooks;
        std::weak_ptr<UiRenderBridge> weak = m_bridge;
        hooks.requestAutoDetect = [weak] {
            if (auto b = weak.lock()) b->autoDetectRequested.store(true);
        };
        hooks.rayTracingAvailable = [weak](std::string& reason) {
            auto b = weak.lock();
            if (!b || !b->hasRenderInfo()) {
                reason = "Renderer not running";
                return false;
            }
            const RenderInfo info = b->renderInfo();
            if (info.caps.rayTracingSupported()) return true;
            reason = info.caps.whyRayTracingUnavailable();
            return false;
        };
        hooks.upscalers = [weak]() -> std::vector<UpscalerAvailability> {
            auto b = weak.lock();
            if (b && b->hasRenderInfo()) return b->renderInfo().upscalers;
            return {{"Off", true, {}}, {"FSR1", true, {}}, {"DLSS", false, "Renderer not running"}};
        };
#if OX_HAS_GLFW
        if (auto* glfw = dynamic_cast<GlfwPlatform*>(engine.platform())) {
            hooks.resolutions = [glfw] {
                std::set<std::pair<int, int>> unique;
                for (const MonitorInfo& m : glfw->monitors())
                    for (const VideoMode& v : m.modes)
                        if (v.size.x >= 1024) unique.insert({v.size.x, v.size.y});
                std::vector<glm::ivec2> out;
                for (auto [w, h] : unique) out.push_back({w, h});
                return out;
            };
        }
#endif
        m_gameUI->bindSettingsMenu(engine.settings(), std::move(hooks));
        m_connections.emplace_back(engine.settings().changed.connect([this](SettingsCategory) {
            if (m_gameUI) m_gameUI->refreshSettingsMenu();
        }));
    }

    Console& console = engine.console();
    console.addCommand("ui.debug", "Toggle the ImGui debug overlay (also F1 / ~)", [this](std::span<const std::string>) {
        if (!m_imgui) return std::string("ImGui disabled");
        m_imgui->toggle();
        return std::string(m_imgui->visible() ? "debug overlay shown" : "debug overlay hidden");
    });
    console.addCommand("ui.open", "ui.open <window>: open a debug window (Stats, Console, ...)", [this](std::span<const std::string> args) {
        if (!m_debugTools || args.empty()) return std::string("usage: ui.open <window>");
        std::string name;
        for (const std::string& a : args) name += (name.empty() ? "" : " ") + a;
        m_debugTools->open(name);
        if (m_imgui) m_imgui->setVisible(true);
        return "opened " + name;
    });
    console.addCommand("ui.load", "ui.load <document.rml>: load and show a game UI document", [this](std::span<const std::string> args) {
        if (!m_gameUI || args.empty()) return std::string("usage: ui.load <document.rml>");
        return std::string(m_gameUI->load(args[0], true) ? "loaded " + args[0] : "failed to load " + args[0]);
    });
    console.addCommand("ui.hide", "ui.hide <document.rml>", [this](std::span<const std::string> args) {
        if (!m_gameUI || args.empty()) return std::string("usage: ui.hide <document.rml>");
        return std::string(m_gameUI->hide(args[0]) ? "hidden" : "unknown document");
    });
    console.addCommand("ui.reload", "Reload every game UI document (.rml/.rcss)", [this](std::span<const std::string>) {
        return m_gameUI ? std::format("reloaded {} documents", m_gameUI->pollHotReload(true)) : std::string("game UI disabled");
    });

#if OX_UI_HAS_SCRIPT
    if (auto* vm = engine.services().tryGet<script::ScriptVM>()) bindLua(*vm);
#endif
}

void UiSystem::detach() {
    if (!m_engine) return;
    m_engine->input().setEventFilter({});
    for (const char* c : {"ui.debug", "ui.open", "ui.load", "ui.hide", "ui.reload"}) m_engine->console().removeCommand(c);
    m_connections.clear();
    m_debugTools.reset();
    if (m_savedCursor >= 0) m_engine->input().setCursorMode(CursorMode(m_savedCursor));
    m_savedCursor = -1;
    m_engine = nullptr;
}

#if OX_UI_HAS_SCRIPT
void UiSystem::bindLua(script::ScriptVM& vm) { bindUiLua(vm, *this, m_lua); }
void UiSystem::unbindLua() { destroyLuaState(m_lua); }
#endif

f32 UiSystem::cursorScale() const {
    if (m_config.cursorScale > 0.0f) return m_config.cursorScale;
#if defined(__APPLE__)
    return m_dpi;
#else
    return 1.0f;
#endif
}

bool UiSystem::processEvent(const InputEvent& event) {
    InputEvent e = event;
    if (e.type == InputEvent::Type::MouseMove) e.position *= cursorScale();
    if (m_imgui && m_imgui->processEvent(e)) return true;
    if (m_gameUI && m_gameUI->processEvent(e)) return true;
    return false;
}

void UiSystem::applyOverlayCursor() {
    if (!m_engine || !m_imgui) return;
    const bool visible = m_imgui->visible();
    if (visible == m_overlayWasVisible) return;
    m_overlayWasVisible = visible;
    InputSystem& input = m_engine->input();
    if (visible) {
        m_savedCursor = int(input.cursorMode());
        input.setCursorMode(CursorMode::Normal);
    } else if (m_savedCursor >= 0) {
        input.setCursorMode(CursorMode(m_savedCursor));
        m_savedCursor = -1;
    }
}

void UiSystem::handleBenchmark() {
    std::optional<render::BenchmarkResult> r = m_bridge->takeBenchmark();
    if (!r) return;
    std::string summary;
    if (r->valid) {
        render::applyQuality(*r);
        // Groups may differ (GI/volumetrics one step lower, RT off without support): report the dominant level.
        std::array<int, kQualityLevelCount> votes{};
        for (QualityLevel l : r->levels)
            if (l != QualityLevel::Custom) ++votes[usize(l)];
        const auto best = QualityLevel(std::max_element(votes.begin(), votes.end()) - votes.begin());
        summary = std::format("Auto: {} (score {:.0f})", scalability::levelName(best), r->score);
    } else {
        summary = "Auto-detect failed";
    }
    OX_LOG_INFO("ui", "{}", summary);
    if (m_engine) m_engine->settings().captureFromCVars();
    if (m_gameUI) m_gameUI->onAutoDetectResult(summary);
    qualityAutoDetected.emit(summary);
}

void UiSystem::beginFrame(f64 dt, glm::uvec2 framebufferSize, f32 dpiScale) {
    OX_PROFILE_ZONE();
    if (m_inFrame) endFrame();
    if (framebufferSize.x == 0 || framebufferSize.y == 0) framebufferSize = m_framebuffer;
    m_framebuffer = framebufferSize;
    m_dpi = dpiScale > 0.0f ? dpiScale : 1.0f;
    handleBenchmark();
    if (m_debugTools) m_debugTools->tick(dt);
    if (m_gameUI) m_gameUI->update(dt, m_framebuffer, m_dpi);
    if (m_imgui) m_imgui->beginFrame(dt, m_framebuffer, m_dpi);
    applyOverlayCursor();
    m_inFrame = true;
}

void UiSystem::endFrame() {
    OX_PROFILE_ZONE();
    if (!m_inFrame) return;
    m_inFrame = false;
    auto frame = std::make_shared<UiFrame>();
    frame->size = m_framebuffer;
    frame->serial = ++m_serial;
    if (m_gameUI) m_gameUI->render(*frame); // game UI below the debug overlay
    if (m_imgui) {
#if OX_UI_HAS_SCRIPT
        closeDanglingLuaWindows(m_lua.get());
#endif
        m_imgui->endFrame(*frame);
    }
    frame->size = m_framebuffer;
    m_lastFrame = frame;
    m_bridge->publish(std::move(frame));
}

} // namespace ox::ui
