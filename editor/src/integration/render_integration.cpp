#include "integration/render_integration.hpp"

#if OX_EDITOR_HAS_RENDER

#include "content/thumbnails.hpp"
#include "core/editor_context.hpp"
#include "core/scene_templates.hpp"
#include "viewport/vulkan_viewport.hpp"

#include <oxwald/core/log.hpp>
#include <oxwald/render/editor_viewport_adapter.hpp>
#include <oxwald/render/features/postprocess/postprocess.hpp>
#include <oxwald/render/features/reflections/reflections.hpp>
#include <oxwald/render/quality.hpp>
#include <oxwald/rhi/device.hpp>
#include <oxwald/scene/components.hpp>
#include <oxwald/scene/prefab.hpp>

#if OX_EDITOR_HAS_ASSETS
#include <oxwald/assets/asset_manager.hpp>
#include <oxwald/assets/mesh.hpp>
#include <oxwald/render/asset_provider.hpp>
#endif

#include <QElapsedTimer>
#include <QPointer>

#include <filesystem>
#include <fstream>
#include <iterator>

#include <glm/gtc/quaternion.hpp>

namespace ox::editor {

// The render module's adapter plus what the editor needs on top: offscreen frames (headless canvas, screenshots,
// thumbnails) and the asset-provider/hot-reload wiring kept per renderer.
class GpuViewportRenderer final : public IViewportRenderer {
public:
    GpuViewportRenderer(RenderIntegration& owner, rhi::Device& device)
        : m_owner(&owner), m_adapter(std::make_unique<render::EditorViewportAdapter>(device)) {}
    ~GpuViewportRenderer() override {
        hotReload = {};
        if (m_owner) m_owner->rendererDestroyed(this);
    }

    [[nodiscard]] QString name() const override { return m_adapter->name(); }
    [[nodiscard]] bool usesPainter() const override { return false; }
    [[nodiscard]] bool supportsViewMode(ViewMode mode) const override { return m_adapter->supportsViewMode(mode); }
    void render(const ViewportFrame& frame, const ViewportTarget& target) override {
        // Same as EditorViewportAdapter::render, with the editor's exposure (ViewportFrame::ev100).
        if (!target.swapchain || !target.commandList) return;
        core().render(convert(frame), target.swapchain->currentTexture(), *target.commandList);
    }
    bool pick(const ViewportFrame& frame, glm::ivec2 pixel, Uuid& out) override {
        const auto hit = core().pick(convert(frame), {frame.sizePx.x, frame.sizePx.y}, pixel);
        out = hit.value_or(Uuid{});
        return true;
    }
    [[nodiscard]] std::vector<GpuPassTiming> passTimings() const override { return m_adapter->passTimings(); }

    QImage renderOffscreen(const ViewportFrame& frame, QSize sizePx) override {
        if (sizePx.isEmpty()) return {};
        const u32 w = u32(sizePx.width()), h = u32(sizePx.height());
        const render::EditorViewportFrame f = convert(frame);
        const std::vector<u8> px = core().renderToImage(f, w, h);
        if (px.size() < usize(w) * h * 4) return {};
        return QImage(px.data(), int(w), int(h), int(w * 4), QImage::Format_RGBA8888).copy();
    }

    render::EditorViewportRenderer& core() { return m_adapter->core(); }

    // Same mapping as render::EditorViewportAdapter (kept here for offscreen frames).
    static render::EditorViewportFrame convert(const ViewportFrame& f) {
        render::EditorViewportFrame e;
        e.world = f.world;
        e.camera.world = glm::inverse(f.view);
        e.camera.projection = f.camera.orthographic ? render::CameraParams::Projection::Orthographic : render::CameraParams::Projection::Perspective;
        e.camera.verticalFov = glm::radians(f.camera.verticalFovDeg);
        e.camera.orthographicHeight = f.camera.orthoHeight;
        e.camera.nearPlane = f.camera.nearPlane;
        e.camera.farPlane = f.camera.farPlane;
        e.camera.ev100 = f.ev100;
        using V = ViewMode;
        switch (f.viewMode) {
        case V::Unlit:
        case V::BufferBaseColor: e.debugView = render::DebugView::Albedo; break;
        case V::Wireframe: e.debugView = render::DebugView::Wireframe; break;
        case V::Normals: e.debugView = render::DebugView::Normals; break;
        case V::Overdraw: e.debugView = render::DebugView::Overdraw; break;
        case V::BufferRoughness: e.debugView = render::DebugView::Roughness; break;
        case V::BufferMetallic: e.debugView = render::DebugView::Metallic; break;
        case V::BufferDepth: e.debugView = render::DebugView::Depth; break;
        case V::BufferMotion: e.debugView = render::DebugView::Velocity; break;
        default: e.debugView = render::DebugView::None; break;
        }
        e.grid = f.showFlags.grid; // the renderer's editor grid (with axis lines) instead of editor-drawn lines
        e.debugDraw = f.showFlags.debugDraw;
        e.shadows = f.showFlags.shadows;
        e.selection = f.selection;
        e.hidden = f.hidden;
        e.lines = f.lines;
        e.time = f.time;
        e.dt = f.dt;
        return e;
    }

    ScopedConnection hotReload;

private:
    QPointer<RenderIntegration> m_owner;
    std::unique_ptr<render::EditorViewportAdapter> m_adapter;
};

namespace {

render::AssetProvider nullProvider() {
    render::AssetProvider p;
    p.loadMesh = [](const Uuid&) { return std::shared_ptr<const assets::MeshData>(); };
    p.loadTexture = [](const Uuid&) { return std::shared_ptr<const assets::TextureData>(); };
    p.loadMaterial = [](const Uuid&) { return std::shared_ptr<const assets::MaterialAsset>(); };
    return p;
}

#if OX_EDITOR_HAS_ASSETS
assets::AssetManager* assetManagerOf(EditorContext& ctx) {
    Services* s = ctx.runtime().services();
    return s ? s->tryGet<assets::AssetManager>() : nullptr;
}
#endif

// Mesh/model/prefab/material previews rendered offscreen with the viewport's renderer.
class RenderThumbnails final : public IThumbnailRenderer {
public:
    explicit RenderThumbnails(RenderIntegration& owner) : m_owner(&owner) {}

    QImage render(const ThumbnailRequest& req) override {
        if (!m_owner) return {};
        GpuViewportRenderer* gpu = m_owner->live();
        if (!gpu) return {};
        EditorContext& ctx = m_owner->context();
        World w;
        AABB bounds;
        const QString& type = req.type;
        if (type == QLatin1String("Material")) {
            Entity e = w.create("Preview");
            auto& mr = e.add<MeshRendererComponent>();
            mr.mesh = builtin::sphereMesh();
            mr.materials = {req.asset};
            bounds = AABB::fromCenterExtents(glm::vec3(0), glm::vec3(0.5f));
        } else if (type == QLatin1String("Mesh")) {
            Entity e = w.create("Preview");
            auto& mr = e.add<MeshRendererComponent>();
            mr.mesh = req.asset;
            mr.materials = {builtin::defaultMaterial()};
            bounds = meshBounds(ctx, req.asset);
        } else if (type == QLatin1String("Model") || type == QLatin1String("Prefab")) {
            QString err;
            auto doc = ctx.services().assets().loadPrefabDocument(req.path, &err);
            if (!doc || !instantiatePrefab(w, *doc)) return {};
            w.updateTransforms();
            for (auto [h, mr] : w.registry().view<MeshRendererComponent>().each()) {
                const AABB local = meshBounds(ctx, mr.mesh);
                if (local.valid()) bounds.expand(local.transformed(w.wrap(h).worldMatrix()));
            }
        } else {
            return {};
        }
        if (!bounds.valid()) bounds = AABB::fromCenterExtents(glm::vec3(0), glm::vec3(0.5f));
        Entity sun = w.create("Sun");
        auto& l = sun.add<LightComponent>();
        l.type = LightType::Directional;
        l.intensity = 40000.0f;
        l.castShadows = false;
        sun.setRotation(glm::quat(glm::vec3(glm::radians(-45.0f), glm::radians(35.0f), 0.0f)));
        Entity env = w.create("Environment");
        auto& ec = env.add<EnvironmentComponent>();
        ec.sun = EntityRef(sun.uuid());
        ec.ambientIntensity = 1.5f;
        w.updateTransforms();

        const glm::vec3 center = bounds.center();
        const float radius = std::max(0.05f, glm::length(bounds.extents()));
        const float fov = 30.0f;
        const glm::vec3 dir = glm::normalize(glm::vec3(1.0f, 0.75f, 1.25f));
        const glm::vec3 eye = center + dir * (radius / std::sin(glm::radians(fov * 0.5f)) * 1.02f);
        render::EditorViewportFrame f;
        f.world = &w;
        f.camera = render::CameraParams::lookAt(eye, center, fov, radius * 0.05f, radius * 40.0f);
        f.camera.ev100 = 14.0f;
        f.grid = false;
        f.debugDraw = false;
        f.selectionOutline = false;
        DebugDraw noLines; // an explicit empty line set (the renderer keeps the previous frame's lines otherwise)
        noLines.flush(0.0f);
        f.lines = &noLines;
        const u32 size = u32(std::clamp(req.sizePx, 32, 512));
        render::EditorViewportRenderer& core = gpu->core();
        (void)core.renderToImage(f, size, size); // first frame requests the assets
        core.renderer().resources().flush();
        const std::vector<u8> px = core.renderToImage(f, size, size);
        if (px.size() < usize(size) * size * 4) return {};
        return QImage(px.data(), int(size), int(size), int(size * 4), QImage::Format_RGBA8888).copy();
    }

private:
    static AABB meshBounds(EditorContext& ctx, const Uuid& mesh) {
        if (!builtin::primitiveName(mesh).isEmpty()) return AABB::fromCenterExtents(glm::vec3(0), glm::vec3(0.5f));
#if OX_EDITOR_HAS_ASSETS
        if (auto* m = assetManagerOf(ctx)) {
            auto h = m->loadSync<assets::MeshData>(mesh);
            if (const assets::MeshData* d = h.get()) return d->bounds;
        }
#else
        (void)ctx;
#endif
        return {};
    }
    QPointer<RenderIntegration> m_owner;
};

// render::autoDetectQuality on the editor's device (or a short-lived headless one) + the CPU index of the heuristic.
class RenderBenchmark final : public IQualityBenchmark {
public:
    [[nodiscard]] QString name() const override { return QStringLiteral("GPU benchmark (render::autoDetectQuality)"); }
    [[nodiscard]] BenchmarkResult run(const RenderingCaps& caps) override {
        HeuristicBenchmark heuristic;
        BenchmarkResult out = heuristic.run(caps);
        render::BenchmarkResult r;
        if (rhi::Device* device = VulkanViewportHub::device()) {
            r = render::autoDetectQuality(*device);
        } else if (!VulkanViewportHub::pending() && !qEnvironmentVariableIsSet("OX_EDITOR_NO_VULKAN")) {
            rhi::DeviceDesc desc;
            desc.appName = "OxwaldEditor benchmark";
            desc.validation = false;
            desc.shaderHotReload = false;
            VulkanViewportHub::prepareDeviceDesc(desc);
            std::string err;
            if (auto device = rhi::Device::create(desc, &err)) r = render::autoDetectQuality(*device);
            else OX_LOG_WARN("editor", "benchmark device: {}", err);
        }
        if (!r.valid) {
            out.details = QStringLiteral("GPU benchmark unavailable, heuristic used.\n") + out.details;
            return out;
        }
        out.gpuIndex = float(r.score);
        out.levels = r.levels;
        // Auto quality incl. AA + upscaler (DLSS on RTX, TAAU/FSR 1 elsewhere).
        if (rhi::Device* device = VulkanViewportHub::device()) {
            const render::RecommendedSettings rec = render::recommendedSettings(*device, r.score);
            out.levels = rec.levels;
            out.antiAliasing = rec.antiAliasing;
            out.upscaler = int(rec.upscaler);
            out.upscalerQuality = int(rec.upscalerQuality);
            out.details = QStringLiteral("CPU index %1 · GPU score %2 · %3").arg(out.cpuIndex, 0, 'f', 0).arg(r.score, 0, 'f', 0).arg(QString::fromStdString(rec.rationale));
            return out;
        }
        out.details = QStringLiteral("CPU index %1 · GPU score %2 (%3)").arg(out.cpuIndex, 0, 'f', 0).arg(r.score, 0, 'f', 0).arg(QString::fromStdString(r.toString()));
        return out;
    }
};

} // namespace

// Upscaler availability from the render module (NGX probe on the live device) on top of the rhi caps.
class RenderCapsProvider final : public IRenderingCapsProvider {
public:
    explicit RenderCapsProvider(std::unique_ptr<IRenderingCapsProvider> base) : m_base(std::move(base)) {}
    [[nodiscard]] RenderingCaps caps() const override {
        RenderingCaps c = m_base->caps();
        rhi::Device* device = VulkanViewportHub::device();
        c.upscalers.clear();
        for (const render::UpscalerAvailability& u : render::upscalerAvailability(device)) {
            const usize i = usize(u.type);
            if (c.upscalers.size() <= i) c.upscalers.resize(i + 1);
            c.upscalers[i] = {QString::fromStdString(u.name), u.available, u.temporal, QString::fromStdString(u.reason)};
            if (u.type == render::UpscalerType::DLSS) {
                c.dlssSupported = u.available;
                c.dlssUnavailableReason = QString::fromStdString(u.reason);
            }
            if (u.type == render::UpscalerType::FSR1) c.fsr1Supported = u.available;
        }
        return c;
    }

private:
    std::unique_ptr<IRenderingCapsProvider> m_base;
};

namespace {
RenderIntegration* g_renderIntegration = nullptr;
} // namespace

RenderIntegration* RenderIntegration::instance() { return g_renderIntegration; }

RenderIntegration::RenderIntegration(EditorContext& ctx) : m_ctx(ctx) {
    g_renderIntegration = this;
    QPointer<RenderIntegration> self(this);
    ctx.services().setCapsProvider(std::make_unique<RenderCapsProvider>(createDefaultCapsProvider()));
    VulkanViewportHub::setDeviceDescHook([](rhi::DeviceDesc& d) { render::appendUpscalerVulkanExtensions(d); });
    ctx.services().setViewportRendererFactory([self](rhi::Device& device) -> std::unique_ptr<IViewportRenderer> {
        return self ? self->createRenderer(device) : nullptr;
    });
    ctx.services().setThumbnailRenderer(std::make_unique<RenderThumbnails>(*this));
    ctx.services().setBenchmark(std::make_unique<RenderBenchmark>());
    connect(&ctx.runtime(), &RuntimeHost::aboutToStop, this, &RenderIntegration::unwireAssets);
    connect(&ctx.runtime(), &RuntimeHost::started, this, [this] {
        for (GpuViewportRenderer* r : m_renderers) {
            wireAssets(*r);
            r->core().resetBakedData(); // project switch / restart: re-read baked probes
        }
    });
}

RenderIntegration::~RenderIntegration() {
    if (g_renderIntegration == this) g_renderIntegration = nullptr;
    unwireAssets();
    m_ctx.services().setViewportRendererFactory({});
    m_ctx.services().setThumbnailRenderer(nullptr);
}

std::unique_ptr<IViewportRenderer> RenderIntegration::createRenderer(rhi::Device& device) {
    auto r = std::make_unique<GpuViewportRenderer>(*this, device);
    QPointer<RenderIntegration> self(this);
    r->core().setBakedDataReader([self](const std::string& file) -> std::optional<std::vector<u8>> {
        if (!self || !self->m_ctx.project()) return std::nullopt;
        const std::filesystem::path path = fsPath(self->m_ctx.project()->rootDir()) /
                                           std::string(render::reflections::kBakedDataDirectory) / file;
        std::ifstream in(path, std::ios::binary);
        if (!in) return std::nullopt;
        return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    });
    m_renderers.push_back(r.get());
    wireAssets(*r);
    ThumbnailCache::instance().clear(); // placeholders cached before the renderer existed
    OX_LOG_INFO("editor", "GPU viewport renderer on {}", device.caps().gpuName);
    return r;
}

void RenderIntegration::rendererDestroyed(GpuViewportRenderer* r) { std::erase(m_renderers, r); }

void RenderIntegration::wireAssets(GpuViewportRenderer& r) {
    auto& cache = r.core().renderer().resources();
#if OX_EDITOR_HAS_ASSETS
    if (assets::AssetManager* m = assetManagerOf(m_ctx)) {
        // Synchronous loads on the UI thread (no JobSystem): the engine's job system dies with the engine on
        // project switches while the renderer lives on.
        cache.setProvider(render::makeAssetManagerProvider(*m), nullptr);
        r.hotReload = render::connectAssetHotReload(*m, cache);
        return;
    }
#endif
    r.hotReload = {};
    cache.setProvider(nullProvider(), nullptr);
}

bool RenderIntegration::bakeProbes(QString& message) {
    GpuViewportRenderer* gpu = live();
    if (!gpu) {
        message = tr("Probe baking needs the GPU viewport renderer");
        return false;
    }
    if (m_ctx.isPlaying()) {
        message = tr("Stop play mode to bake probes");
        return false;
    }
    if (!m_ctx.project()) {
        message = tr("Open a project to bake probes");
        return false;
    }
    QElapsedTimer t;
    t.start();
    render::EditorViewportRenderer& core = gpu->core();
    render::EditorViewportFrame f;
    f.world = &m_ctx.editWorld();
    f.camera = render::CameraParams::lookAt({0.0f, 2.0f, 8.0f}, {0.0f, 0.0f, 0.0f});
    f.grid = false;
    f.debugDraw = false;
    f.selectionOutline = false;
    DebugDraw noLines;
    noLines.flush(0.0f);
    f.lines = &noLines;
    m_ctx.editWorld().updateTransforms();
    render::reflections::requestBake(core.renderer());
    // Captures run within the per-frame budgets; offscreen frames drive them (the camera does not matter).
    int frames = 0;
    for (; frames < 2000 && (frames < 2 || render::reflections::bakeInProgress(core.renderer())); ++frames) {
        (void)core.renderToImage(f, 64, 64);
    }
    if (render::reflections::bakeInProgress(core.renderer())) {
        message = tr("Probe bake did not finish");
        return false;
    }
    const std::filesystem::path dir = fsPath(m_ctx.project()->rootDir()) /
                                      std::string(render::reflections::kBakedDataDirectory);
    auto written = render::reflections::saveBakedData(core.renderer(), dir);
    if (!written) {
        message = tr("Saving baked probes failed: %1").arg(QString::fromStdString(written.error().message));
        return false;
    }
    message = tr("Baked %1 probe file(s) in %2 ms (%3 frames)").arg(*written).arg(t.elapsed()).arg(frames);
    return true;
}

void RenderIntegration::unwireAssets() {
    for (GpuViewportRenderer* r : m_renderers) {
        r->hotReload = {};
        r->core().renderer().resources().setProvider(nullProvider(), nullptr);
    }
}

} // namespace ox::editor

#endif
