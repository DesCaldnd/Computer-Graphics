#include <oxwald/core/cvar.hpp>
#include <oxwald/core/debug_draw.hpp>
#include <oxwald/core/jobs.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/core/profile.hpp>
#include <oxwald/core/services.hpp>
#include <oxwald/render/features/postprocess/postprocess.hpp>
#include <oxwald/render/quality.hpp>
#include <oxwald/render/renderer.hpp>
#include <oxwald/render/runtime_renderer.hpp>
#include <oxwald/rhi/device.hpp>
#include <oxwald/rhi/swapchain.hpp>
#include <oxwald/scene/world.hpp>
#if OX_RENDER_HAS_ASSETS
#include <oxwald/assets/asset_manager.hpp>
#include <oxwald/render/asset_provider.hpp>
#endif

#include <array>
#include <atomic>
#include <chrono>

namespace ox::render {

namespace {

class RuntimeRenderer final : public IRenderer {
public:
    explicit RuntimeRenderer(RuntimeRendererOptions o) : m_options(o) {}
    ~RuntimeRenderer() override { shutdown(); }

    std::string_view name() const override { return "Vulkan"; }

    Status init(Services& services, const RenderSurface& surface) override {
        rhi::DeviceDesc dd;
        dd.appName = "OxwaldEngine";
        dd.surface = surface.provider;
        appendUpscalerVulkanExtensions(dd); // NGX (DLSS) extensions where DLSS can run
        std::string error;
        m_device = rhi::Device::create(dd, &error);
        if (!m_device) return makeError("Vulkan device creation failed: {}", error);
        if (surface.provider) {
            m_swapchain = rhi::Swapchain::create(*m_device, {presentMode()});
            if (!m_swapchain) return makeError("swapchain creation failed");
        } else {
            rhi::TextureDesc td;
            td.name = "runtime.offscreen";
            td.format = VK_FORMAT_R8G8B8A8_UNORM;
            td.width = m_options.headlessWidth;
            td.height = m_options.headlessHeight;
            td.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferSrc;
            m_offscreen = m_device->createTexture(td);
        }
        if (m_options.autoDetectQuality) {
            // Scalability levels + AA/upscaler (DLSS on RTX, TAAU/FSR 1 on slower GPUs, see recommendedSettings()).
            const BenchmarkResult bench = autoDetectQuality(*m_device);
            if (bench.valid) applyRecommendedSettings(recommendedSettings(*m_device, bench.score));
        }
        RendererDesc rd;
        rd.jobs = services.tryGet<JobSystem>();
        m_renderer = Renderer::create(*m_device, rd);
        m_view = m_renderer->createView({.name = "Main"});
        m_debugDraw = services.tryGet<DebugDraw>();
        m_services = &services;
#if OX_RENDER_HAS_ASSETS
        if (auto* am = services.tryGet<assets::AssetManager>()) {
            m_renderer->resources().setProvider(makeAssetManagerProvider(*am), rd.jobs);
            m_hotReload = connectAssetHotReload(*am, m_renderer->resources());
        }
#endif
        OX_LOG_INFO("render", "renderer initialised ({}, {})", m_device->caps().gpuName,
                    surface.provider ? "windowed" : "headless");
        return {};
    }

    void shutdown() override {
        if (!m_device) return;
        m_device->waitIdle();
#if OX_RENDER_HAS_ASSETS
        m_hotReload = {};
#endif
        m_renderer.reset();
        m_swapchain.reset();
        if (m_offscreen) m_device->destroy(m_offscreen);
        m_offscreen = {};
        m_device.reset();
    }

    void extract(const World& world, const FrameContext& ctx) override {
        OX_PROFILE_ZONE();
        const auto t0 = std::chrono::steady_clock::now();
        if (m_debugDraw) m_debugDraw->flush(ctx.dt);
        RenderSnapshot& slot = m_slots[ctx.slot % kRenderSnapshotSlots];
        render::extract(world, slot,
                        {.debugDraw = m_debugDraw, .time = ctx.time, .deltaTime = ctx.dt, .frame = ctx.frameIndex,
                         .services = m_services});
        m_lastExtractMs.store(std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }

    void render(const FrameContext& ctx) override {
        OX_PROFILE_ZONE();
        if (!m_device) return;
        const auto t0 = std::chrono::steady_clock::now();
        const RenderSnapshot& snap = m_slots[ctx.slot % kRenderSnapshotSlots];
        m_device->beginFrame();
        if (m_swapchain && !m_swapchain->acquire()) { // minimised
            m_device->endFrame();
            return;
        }
        m_renderer->beginFrame(snap);
        ViewRenderRequest req;
        req.view = m_view;
        if (const i32 c = snap.primaryCamera(); c >= 0) {
            req.camera = CameraParams::fromComponent(snap.cameras[usize(c)].camera, snap.cameras[usize(c)].world);
        } else {
            req.camera = CameraParams::lookAt({0, 2, 8}, {0, 0, 0});
            req.camera.ev100 = 12.0f;
        }
        if (m_swapchain) req.target.swapchain = m_swapchain.get();
        else req.target.texture = m_offscreen;
        m_renderer->renderView(req);
        m_renderer->endFrame();
        if (m_swapchain) m_swapchain->present();
        m_device->endFrame();
        m_frames.fetch_add(1);
        m_drawCalls.store(m_renderer->stats().drawCalls);
        m_lastRenderMs.store(std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }

    void resize(glm::uvec2 size) override {
        m_size = size;
        if (m_swapchain && size.x > 0 && size.y > 0) m_swapchain->recreate();
    }

    void settingsChanged() override {
        // Render cvars are re-read every frame (graph rebuilds automatically); only presentation needs a push.
        if (m_swapchain) m_swapchain->setPresentMode(presentMode());
    }

    ox::RenderStats stats() const override {
        ox::RenderStats s;
        s.framesRendered = m_frames.load();
        s.lastRenderMs = m_lastRenderMs.load();
        s.lastExtractMs = m_lastExtractMs.load();
        s.drawCalls = m_drawCalls.load();
        return s;
    }

    Renderer* renderer() { return m_renderer.get(); }

private:
    static rhi::PresentMode presentMode() {
        // r.VSync is owned by the runtime; read it by name.
        if (auto* v = CVarRegistry::instance().findAs<bool>("r.VSync"); v && !v->get()) return rhi::PresentMode::Mailbox;
        return rhi::PresentMode::VSync;
    }

    RuntimeRendererOptions m_options;
    std::unique_ptr<rhi::Device> m_device;
    std::unique_ptr<rhi::Swapchain> m_swapchain;
    std::unique_ptr<Renderer> m_renderer;
    rhi::TextureHandle m_offscreen;
    ViewId m_view = 0;
    DebugDraw* m_debugDraw = nullptr;
    Services* m_services = nullptr; // ExtractHookEx hooks (gameplay WorldRenderData bridge)
    std::array<RenderSnapshot, kRenderSnapshotSlots> m_slots;
    glm::uvec2 m_size{0, 0};
    std::atomic<u64> m_frames{0};
    std::atomic<u64> m_drawCalls{0};
    std::atomic<f64> m_lastRenderMs{0.0};
    std::atomic<f64> m_lastExtractMs{0.0};
#if OX_RENDER_HAS_ASSETS
    ScopedConnection m_hotReload;
#endif
};

} // namespace

std::unique_ptr<IRenderer> createRenderer(const RuntimeRendererOptions& options) {
    return std::make_unique<RuntimeRenderer>(options);
}

Renderer* rendererOf(IRenderer& renderer) {
    auto* r = dynamic_cast<RuntimeRenderer*>(&renderer);
    return r ? r->renderer() : nullptr;
}

} // namespace ox::render
