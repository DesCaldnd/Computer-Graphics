#include <oxwald/ui/ui_module.hpp>

#include <oxwald/core/log.hpp>
#include <oxwald/core/services.hpp>
#include <oxwald/core/vfs.hpp>
#include <oxwald/render/quality.hpp>
#include <oxwald/render/renderer.hpp>
#include <oxwald/runtime/platform.hpp>
#include <oxwald/ui/ui_render_feature.hpp>
#include <oxwald/rhi/device.hpp>

#if OX_RENDER_HAS_RUNTIME
#include <oxwald/render/runtime_renderer.hpp>
#endif

namespace ox::ui {

Status UiModule::init(Engine& engine, Services& services) {
    if (m_config.imguiConfig.iniPath.empty()) {
        // Debug window layout persists per user (user://imgui.ini).
        if (auto p = engine.vfs().resolveNative("user://imgui.ini")) m_config.imguiConfig.iniPath = p->string();
    }
    m_system = &services.emplace<UiSystem>(m_config, &engine.vfs());
    m_system->attach(engine);
    m_frameEnded = engine.frameEnded.connect([this](const EngineStats&) {
        if (m_system) m_system->endFrame();
    });
    OX_LOG_INFO("ui", "UI module initialised (ImGui {}, RmlUi {})", m_system->imgui() ? "on" : "off",
                m_system->gameUI() ? "on" : "off");
    return {};
}

void UiModule::preUpdate(Engine& engine, const FrameTime& time) {
    if (!m_system) return;
    glm::uvec2 fb = m_config.headlessSize;
    f32 dpi = 1.0f;
    if (IPlatform* p = engine.platform()) {
        fb = p->framebufferSize();
        dpi = p->surface().dpiScale;
    }
    m_system->beginFrame(time.realDt, fb, dpi);
}

void UiModule::shutdown(Engine& engine, Services& services) {
    m_frameEnded = {};
    if (m_system) m_system->detach();
    m_system = nullptr;
}

namespace {

class UiRendererDecorator final : public IRenderer {
public:
    explicit UiRendererDecorator(std::unique_ptr<IRenderer> inner) : m_inner(std::move(inner)) {}

    std::string_view name() const override { return m_inner->name(); }

    Status init(Services& services, const RenderSurface& surface) override {
        Status st = m_inner->init(services, surface);
        if (!st) return st;
#if OX_RENDER_HAS_RUNTIME
        if (auto* ui = services.tryGet<UiSystem>()) {
            m_renderer = render::rendererOf(*m_inner);
            if (m_renderer) {
                m_bridge = ui->bridgeShared();
                attachRenderer(*m_renderer, m_bridge);
            } else {
                OX_LOG_WARN("ui", "withUi: renderer '{}' is not the render module's renderer; UI is not drawn", m_inner->name());
            }
        } else {
            OX_LOG_WARN("ui", "withUi: no UiSystem service (add ox::ui::UiModule); UI is not drawn");
        }
#endif
        return st;
    }
    void shutdown() override {
        m_renderer = nullptr;
        m_inner->shutdown();
    }
    void extract(const World& world, const FrameContext& ctx) override { m_inner->extract(world, ctx); }
    void render(const FrameContext& ctx) override {
        // Between two device frames on the render thread: the only safe place for the benchmark's own frames.
        if (m_renderer && m_bridge && m_bridge->autoDetectRequested.exchange(false)) {
            m_bridge->setBenchmark(render::autoDetectQuality(m_renderer->device()));
        }
        m_inner->render(ctx);
    }
    void resize(glm::uvec2 size) override { m_inner->resize(size); }
    void settingsChanged() override { m_inner->settingsChanged(); }
    RenderStats stats() const override { return m_inner->stats(); }

private:
    std::unique_ptr<IRenderer> m_inner;
    render::Renderer* m_renderer = nullptr;
    std::shared_ptr<UiRenderBridge> m_bridge;
};

} // namespace

std::unique_ptr<IRenderer> withUi(std::unique_ptr<IRenderer> renderer) {
    return std::make_unique<UiRendererDecorator>(std::move(renderer));
}

} // namespace ox::ui
