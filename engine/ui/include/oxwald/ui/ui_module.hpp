#pragma once

// Engine integration of the ui module:
//
//   ox::Engine engine;
//   engine.addModule(std::make_unique<ox::ui::UiModule>());                     // UiSystem service + input + Lua
//   engine.setRenderer(ox::ui::withUi(ox::render::createRenderer()));           // draws the UI, runs "Auto" quality
//
// UiModule::init registers ox::ui::UiSystem in Services and attaches it (InputSystem filter: ImGui overlay ->
// game UI -> game; debug windows; settings menu model; Lua `ui` / `debug` tables; console commands ui.*).
// Every frame: preUpdate begins the UI frame (ImGui NewFrame, RmlUi update), Engine::frameEnded ends it and
// publishes the UiFrame to the renderer. withUi() adds the UI render feature once the renderer exists and runs the
// quality benchmark between frames when the settings menu asks for "Auto".

#include <oxwald/runtime/engine.hpp>
#include <oxwald/runtime/renderer.hpp>
#include <oxwald/ui/ui_system.hpp>

#include <memory>

namespace ox::ui {

class UiModule final : public IEngineModule {
public:
    explicit UiModule(UiConfig config = {}) : m_config(std::move(config)) {}

    [[nodiscard]] std::string_view name() const override { return "UI"; }
    Status init(Engine& engine, Services& services) override;
    void preUpdate(Engine& engine, const FrameTime& time) override;
    void shutdown(Engine& engine, Services& services) override;

    [[nodiscard]] UiSystem* system() const { return m_system; }

private:
    UiConfig m_config;
    UiSystem* m_system = nullptr;
    ScopedConnection m_frameEnded;
};

// Wraps a renderer created by render::createRenderer(): after init() the UI overlay feature is attached to its
// Renderer (needs the UiSystem service, i.e. UiModule). Other IRenderer implementations pass through unchanged.
[[nodiscard]] std::unique_ptr<IRenderer> withUi(std::unique_ptr<IRenderer> renderer);

} // namespace ox::ui
