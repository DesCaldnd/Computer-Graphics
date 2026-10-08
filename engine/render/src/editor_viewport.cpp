#include <oxwald/core/log.hpp>
#include <oxwald/render/editor_viewport.hpp>
#include <oxwald/render/features/reflections/reflections.hpp>
#include <oxwald/rhi/device.hpp>
#include <oxwald/scene/world.hpp>

#include <unordered_set>

namespace ox::render {

EditorViewportRenderer::EditorViewportRenderer(rhi::Device& device, const RendererDesc& desc) : m_device(&device) {
    m_renderer = Renderer::create(device, desc);
    ViewDesc vd;
    vd.name = "EditorViewport";
    vd.flags.editor = true;
    vd.flags.grid = true;
    m_view = m_renderer->createView(vd);
}

EditorViewportRenderer::~EditorViewportRenderer() {
    m_device->waitIdle();
    m_renderer.reset();
    if (m_offscreen) m_device->destroy(m_offscreen);
}

void EditorViewportRenderer::buildSnapshot(const EditorViewportFrame& f) {
    m_snapshot.selection.clear();
    if (!f.world) {
        m_snapshot.clear();
        return;
    }
    // DebugDraw is const here: extract only reads the already flushed spans.
    extract(*f.world, m_snapshot, {.debugDraw = const_cast<DebugDraw*>(f.lines), .time = f.time, .deltaTime = f.dt});
    auto idOf = [&](const Uuid& u) -> u32 {
        const Entity e = f.world->find(u);
        return e ? encodeEntityId(u32(entt::to_integral(e.handle()))) : kNoEntity;
    };
    for (const Uuid& u : f.selection) {
        if (const u32 id = idOf(u)) m_snapshot.selection.push_back(id);
    }
    if (!f.hidden.empty()) {
        std::unordered_set<u32> hidden;
        for (const Uuid& u : f.hidden) hidden.insert(idOf(u));
        std::erase_if(m_snapshot.meshes, [&](const SnapshotMesh& m) { return hidden.count(m.entityId) != 0; });
        std::erase_if(m_snapshot.lights, [&](const SnapshotLight& l) { return hidden.count(l.entityId) != 0; });
        if (m_snapshot.environment) m_snapshot.environment->sunLight = -1; // indices changed; re-resolved below
        for (usize i = 0; i < m_snapshot.lights.size() && m_snapshot.environment; ++i) {
            if (m_snapshot.lights[i].light.type == LightType::Directional) {
                m_snapshot.environment->sunLight = i32(i);
                break;
            }
        }
    }
}

RenderSettings EditorViewportRenderer::settingsFor(const EditorViewportFrame& f) const {
    RenderSettings s = RenderSettings::fromCVars();
    s.debugView = f.debugView;
    s.wireframe = f.wireframe;
    s.shadows = s.shadows && f.shadows;
    return s;
}

void EditorViewportRenderer::setBakedDataReader(std::function<std::optional<std::vector<u8>>(const std::string&)> reader) {
    m_bakedReader = std::move(reader);
    m_bakedAttempted.clear();
}

void EditorViewportRenderer::applyViewFlags(const EditorViewportFrame& f) {
    if (m_bakedReader) reflections::installBakedData(*m_renderer, m_snapshot, m_bakedReader, m_bakedAttempted);
    RenderView* v = m_renderer->view(m_view);
    v->desc().flags.grid = f.grid;
    v->desc().flags.debugDraw = f.debugDraw;
    v->desc().flags.selectionOutline = f.selectionOutline;
}

void EditorViewportRenderer::render(const EditorViewportFrame& f, rhi::TextureHandle target, rhi::CommandList& cmd) {
    buildSnapshot(f);
    applyViewFlags(f);
    m_renderer->beginFrame(m_snapshot);
    ViewRenderRequest req;
    req.view = m_view;
    req.camera = f.camera;
    req.target.texture = target;
    req.target.finalAccess = rhi::Access::ColorAttachmentWrite;
    req.settingsOverride = settingsFor(f);
    req.recordInto = &cmd;
    m_renderer->renderView(req);
    m_renderer->endFrame();
}

rhi::TextureHandle EditorViewportRenderer::offscreen(u32 w, u32 h) {
    if (m_offscreen) {
        const rhi::TextureDesc& d = m_device->desc(m_offscreen);
        if (d.width == w && d.height == h) return m_offscreen;
        m_device->destroy(m_offscreen);
    }
    rhi::TextureDesc d;
    d.name = "editor.offscreen";
    d.format = VK_FORMAT_R8G8B8A8_UNORM;
    d.width = w;
    d.height = h;
    d.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferSrc;
    m_offscreen = m_device->createTexture(d);
    return m_offscreen;
}

std::vector<u8> EditorViewportRenderer::renderToImage(const EditorViewportFrame& f, u32 w, u32 h) {
    const rhi::TextureHandle t = offscreen(w, h);
    buildSnapshot(f);
    applyViewFlags(f); // same show flags as on screen (thumbnails/screenshots used to keep the previous ones)
    m_device->beginFrame();
    m_renderer->beginFrame(m_snapshot);
    ViewRenderRequest req;
    req.view = m_view;
    req.camera = f.camera;
    req.target.texture = t;
    req.target.finalAccess = rhi::Access::TransferRead;
    req.settingsOverride = settingsFor(f);
    m_renderer->renderView(req);
    m_renderer->endFrame();
    m_device->endFrame();
    m_device->waitIdle();
    return m_device->readTexture(t);
}

std::optional<Uuid> EditorViewportRenderer::pick(const EditorViewportFrame& f, Extent2D size, glm::ivec2 pixel) {
    if (!f.world || pixel.x < 0 || pixel.y < 0 || u32(pixel.x) >= size.width || u32(pixel.y) >= size.height) return std::nullopt;
    const PickRequestId id = m_renderer->requestPick(m_view, u32(pixel.x), u32(pixel.y));
    renderToImage(f, size.width, size.height); // records the pick
    // The readback completes with that submission; collect it (another frame is not needed after waitIdle).
    PickResult r = m_renderer->takePickResult(id);
    if (!r.ready) {
        renderToImage(f, size.width, size.height);
        r = m_renderer->takePickResult(id);
    }
    if (!r.ready || r.ids.empty() || r.ids[0] == kNoEntity) return std::nullopt;
    const entt::entity e = entt::entity(decodeEntityId(r.ids[0]));
    const entt::registry& reg = f.world->registry();
    if (!reg.valid(e)) return std::nullopt;
    if (const auto* idc = reg.try_get<IdComponent>(e)) return idc->id;
    return std::nullopt;
}

} // namespace ox::render
