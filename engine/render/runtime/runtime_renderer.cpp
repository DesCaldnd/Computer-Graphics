#include <oxwald/core/cvar.hpp>
#include <oxwald/core/debug_draw.hpp>
#include <oxwald/core/jobs.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/core/paths.hpp>
#include <oxwald/core/profile.hpp>
#include <oxwald/core/services.hpp>
#include <oxwald/core/vfs.hpp>
#include <oxwald/render/features/postprocess/postprocess.hpp>
#include <oxwald/render/features/reflections/reflections.hpp>
#include <oxwald/render/quality.hpp>
#include <oxwald/render/register_types.hpp>
#include <oxwald/render/renderer.hpp>
#include <oxwald/render/runtime_renderer.hpp>
#include <oxwald/rhi/device.hpp>
#include <oxwald/runtime/settings.hpp>
#include <oxwald/rhi/swapchain.hpp>
#include <oxwald/scene/scene.hpp>
#include <oxwald/scene/world.hpp>
#if OX_RENDER_HAS_ASSETS
#include <oxwald/assets/asset_manager.hpp>
#include <oxwald/assets/image.hpp>
#include <oxwald/render/asset_provider.hpp>
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <unordered_set>

namespace ox::render {

namespace {

// user://cache/pipeline_cache.bin (falls back to the platform user data dir without a VFS); saved on shutdown.
std::filesystem::path pipelineCachePath(Services& services) {
    if (const Vfs* vfs = services.tryGet<Vfs>()) {
        if (auto file = vfs->resolveNative("user://cache/pipeline_cache.bin")) return *file;
    }
    return paths::userDataDir() / "cache" / "pipeline_cache.bin";
}

class RuntimeRenderer final : public IRenderer {
public:
    explicit RuntimeRenderer(RuntimeRendererOptions o) : m_options(o) {}
    ~RuntimeRenderer() override { shutdown(); }

    std::string_view name() const override { return "Vulkan"; }

    Status init(Services& services, const RenderSurface& surface) override {
        rhi::DeviceDesc dd;
        dd.appName = "OxwaldEngine";
        dd.surface = surface.provider;
        dd.pipelineCachePath = pipelineCachePath(services);
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
        // Only on first launch: afterwards the player's own (saved) quality settings must win.
        Settings* userSettings = services.tryGet<Settings>();
        if (m_options.autoDetectQuality && (!userSettings || !userSettings->hasUserFile())) {
            // Scalability levels + AA/upscaler (DLSS on RTX, TAAU/FSR 1 on slower GPUs, see recommendedSettings()).
            const BenchmarkResult bench = autoDetectQuality(*m_device);
            if (bench.valid) {
                applyRecommendedSettings(recommendedSettings(*m_device, bench.score));
                if (userSettings) userSettings->captureFromCVars(); // saved on shutdown -> not detected again
            }
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
        if (m_capture) m_device->destroy(m_capture);
        m_capture = {};
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
        installBakedProbes(snap);
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
        // Windowed screenshot: the same view (same exposure/TAA state) is also rendered into a capture texture of
        // the swapchain's size right before the presented image.
        rhi::TextureHandle capture = m_offscreen;
        if (m_swapchain && screenshotPending()) {
            capture = captureTexture(m_swapchain->extent());
            ViewRenderRequest creq = req;
            creq.target.texture = capture;
            creq.target.finalAccess = rhi::Access::TransferRead;
            m_renderer->renderView(creq);
        }
        if (m_swapchain) req.target.swapchain = m_swapchain.get();
        else req.target.texture = m_offscreen;
        m_renderer->renderView(req);
        m_renderer->endFrame();
        if (m_swapchain) m_swapchain->present();
        m_device->endFrame();
        writePendingScreenshot(capture);
        m_frames.fetch_add(1);
        m_drawCalls.store(m_renderer->stats().drawCalls);
        m_lastRenderMs.store(std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }

    // Baked reflection probes / irradiance volumes of newly seen entities: project://Baked/<uuid>.oxcube|.oxirr
    // (loose project files or the cooked pak). Render thread, before the frame.
    void installBakedProbes(const RenderSnapshot& snap) {
        Vfs* vfs = m_services ? m_services->tryGet<Vfs>() : nullptr;
        if (!vfs) return;
        reflections::installBakedData(
            *m_renderer, snap,
            [vfs](const std::string& file) -> std::optional<std::vector<u8>> {
                const std::string uri = "project://" + std::string(reflections::kBakedDataDirectory) + "/" + file;
                if (!vfs->exists(uri)) return std::nullopt;
                auto bytes = vfs->readBytes(uri);
                if (!bytes) return std::nullopt;
                std::vector<u8> out(bytes->size());
                if (!out.empty()) std::memcpy(out.data(), bytes->data(), out.size());
                return out;
            },
            m_bakedAttempted);
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

    bool requestScreenshot(std::filesystem::path path, std::function<void(bool)> done) override {
        std::lock_guard lock(m_screenshotMutex);
        m_screenshots.push_back({std::move(path), std::move(done)});
        return true;
    }

private:
    struct ScreenshotRequest {
        std::filesystem::path path;
        std::function<void(bool)> done;
    };

    bool screenshotPending() {
        std::lock_guard lock(m_screenshotMutex);
        return !m_screenshots.empty();
    }

    rhi::TextureHandle captureTexture(VkExtent2D size) {
        if (m_capture) {
            const rhi::TextureDesc& d = m_device->desc(m_capture);
            if (d.width == size.width && d.height == size.height) return m_capture;
            m_device->destroy(m_capture);
        }
        rhi::TextureDesc td;
        td.name = "runtime.screenshot";
        td.format = VK_FORMAT_R8G8B8A8_UNORM;
        td.width = std::max(size.width, 1u);
        td.height = std::max(size.height, 1u);
        td.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferSrc;
        m_capture = m_device->createTexture(td);
        return m_capture;
    }

    void writePendingScreenshot(rhi::TextureHandle source) {
        std::vector<ScreenshotRequest> requests;
        {
            std::lock_guard lock(m_screenshotMutex);
            requests.swap(m_screenshots);
        }
        if (requests.empty()) return;
        bool ok = false;
#if OX_RENDER_HAS_ASSETS
        if (!source) {
            for (const ScreenshotRequest& r : requests) {
                if (r.done) r.done(false);
            }
            return;
        }
        m_device->waitIdle();
        const std::vector<u8> rgba = m_device->readTexture(source);
        const rhi::TextureDesc& d = m_device->desc(source);
        assets::Image img(d.width, d.height);
        for (usize i = 0; i < img.pixels.size() && i * 4 + 3 < rgba.size(); ++i) {
            img.pixels[i] = glm::vec4(rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2], 255.0f) / 255.0f;
        }
        for (const ScreenshotRequest& r : requests) {
            std::error_code ec;
            if (r.path.has_parent_path()) std::filesystem::create_directories(r.path.parent_path(), ec);
            const Status st = assets::saveImagePng(r.path, img);
            if (!st) OX_LOG_ERROR("render", "screenshot {}: {}", r.path.string(), st.error().message);
            else OX_LOG_INFO("render", "screenshot written: {}", r.path.string());
            ok = bool(st);
            if (r.done) r.done(ok);
        }
#else
        for (const ScreenshotRequest& r : requests) {
            OX_LOG_ERROR("render", "screenshot {}: built without the assets module (no PNG writer)", r.path.string());
            if (r.done) r.done(ok);
        }
#endif
    }

    static rhi::PresentMode presentMode() {
        // r.VSync is owned by the runtime; read it by name.
        if (auto* v = CVarRegistry::instance().findAs<bool>("r.VSync"); v && !v->get()) return rhi::PresentMode::Mailbox;
        return rhi::PresentMode::VSync;
    }

    std::unordered_set<Uuid> m_bakedAttempted;
    RuntimeRendererOptions m_options;
    std::unique_ptr<rhi::Device> m_device;
    std::unique_ptr<rhi::Swapchain> m_swapchain;
    std::unique_ptr<Renderer> m_renderer;
    rhi::TextureHandle m_offscreen;
    rhi::TextureHandle m_capture; // windowed screenshots
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
    std::mutex m_screenshotMutex;
    std::vector<ScreenshotRequest> m_screenshots;
};

} // namespace

std::unique_ptr<IRenderer> createRenderer(const RuntimeRendererOptions& options) {
    // Render components (post-process/fog volumes, probes, particles, water, ...) must be reflected before the
    // engine loads the startup scene, which happens in Engine::init before the renderer is initialised.
    registerSceneTypes();
    registerRenderTypes();
    return std::make_unique<RuntimeRenderer>(options);
}

Renderer* rendererOf(IRenderer& renderer) {
    auto* r = dynamic_cast<RuntimeRenderer*>(&renderer);
    return r ? r->renderer() : nullptr;
}

} // namespace ox::render
