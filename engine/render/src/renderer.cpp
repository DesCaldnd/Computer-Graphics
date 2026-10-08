#include "renderer_impl.hpp"

#include "features/gpu_driven/gpu_driven.hpp"
#include "features/gpu_driven/texture_streaming.hpp"

#include <oxwald/render/features/gpu_driven/gpu_driven.hpp>

#include <oxwald/core/jobs.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/core/profile.hpp>
#include <oxwald/render/clusters.hpp>
#include <oxwald/render/features/postprocess/postprocess.hpp>
#include <oxwald/render/features/raytracing/raytracing.hpp>
#include <oxwald/render/features/reflections/reflections.hpp>
#include <oxwald/render/features/translucency/translucency.hpp>
#include <oxwald/render/features/volumetrics/volumetrics.hpp>
#include <oxwald/render/features/world/world_skinning.hpp>
#include <oxwald/rhi/swapchain.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace ox::render {

namespace {

struct DrawPush {
    u64 view = 0;
    u64 scene = 0;
    u64 drawIds = 0;
    u32 inputs[6] = {kInvalidIndex, kInvalidIndex, kInvalidIndex, kInvalidIndex, 0, 0};
};
static_assert(sizeof(DrawPush) == 48);

struct ViewScenePush {
    u64 view = 0;
    u64 scene = 0;
    u32 unused = 0;
};

constexpr u32 kClusterX = 16, kClusterY = 9, kClusterZ = 24;

rhi::TextureDesc targetDesc(VkFormat format, Extent2D e, const char* name, u32 mips = 1) {
    rhi::TextureDesc d;
    d.format = format;
    d.width = std::max(e.width, 1u);
    d.height = std::max(e.height, 1u);
    d.mipLevels = mips;
    d.usage = rhi::TextureUsage::None;
    d.name = name;
    return d;
}

bool isSrgb(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return true;
    default: return false;
    }
}

} // namespace

// --- GpuFrameAllocator ---

void GpuFrameAllocator::init(rhi::Device& device, u32 framesInFlight, u64 chunkSize) {
    m_device = &device;
    m_frames.resize(std::max(framesInFlight, 1u));
    m_chunkSize = chunkSize;
}

void GpuFrameAllocator::release(rhi::Device& device) {
    for (auto& f : m_frames) {
        for (Chunk& c : f) device.destroy(c.buffer);
        f.clear();
    }
}

void GpuFrameAllocator::beginFrame(u32 frameSlot) {
    m_slot = frameSlot % u32(m_frames.size());
    for (Chunk& c : m_frames[m_slot]) c.used = 0;
}

GpuAllocation GpuFrameAllocator::allocate(u64 size, u64 alignment) {
    size = std::max<u64>(size, 4);
    alignment = std::max<u64>(alignment, 16);
    auto& chunks = m_frames[m_slot];
    for (Chunk& c : chunks) {
        const u64 off = (c.used + alignment - 1) & ~(alignment - 1);
        if (off + size <= c.size) {
            c.used = off + size;
            return {c.cpu + off, c.address + off, size, c.buffer, off};
        }
    }
    Chunk c;
    c.size = std::max(m_chunkSize, size);
    using U = rhi::BufferUsage;
    c.buffer = m_device->createBuffer({c.size, U::Storage | U::TransferSrc | U::Uniform | U::Index | U::Vertex | U::Indirect,
                                       rhi::MemoryUsage::Upload, "render.frameUpload"});
    c.cpu = static_cast<u8*>(m_device->mapped(c.buffer));
    c.address = m_device->address(c.buffer);
    c.used = size;
    chunks.push_back(c);
    return {c.cpu, c.address, size, c.buffer, 0};
}

u64 GpuFrameAllocator::usedThisFrame() const {
    u64 u = 0;
    for (const Chunk& c : m_frames[m_slot]) u += c.used;
    return u;
}

// --- Renderer ---

Renderer::Renderer() : m_impl(std::make_unique<Impl>()) {}

std::unique_ptr<Renderer> Renderer::create(rhi::Device& device, const RendererDesc& desc) {
    registerRenderCVars();
    std::unique_ptr<Renderer> r(new Renderer());
    Impl& m = *r->m_impl;
    m.self = r.get();
    m.device = &device;
    m.desc = desc;
    m.scene = std::make_unique<GpuScene>(device);
    m.cache = std::make_unique<GpuResourceCache>(device, *m.scene);
    m.gpuDriven = std::make_unique<GpuDriven>(device, *m.scene, m.frameAlloc, desc.jobs);
    m.streamer = std::make_unique<TextureStreamer>(device, *m.cache, *m.scene);
    m.cache->setStreamingHook(m.streamer.get());
    if (desc.jobs) m.cache->setProvider({}, desc.jobs);
    m.frameAlloc.init(device, device.framesInFlight());
    const std::array<glm::vec4, 9> zero{};
    m.zeroSH = device.createBuffer({sizeof(zero), rhi::BufferUsage::Storage, rhi::MemoryUsage::GpuOnly, "render.zeroSH"},
                                   zero.data());
    m.createPipelines();
    if (desc.builtinFeatures) registerBuiltinFeatures(*r);
    if (desc.instantiateFactories) {
        for (auto& [name, factory] : featureFactories()) {
            if (auto f = factory()) m.features.add(std::move(f));
        }
    }
    return r;
}

Renderer::~Renderer() {
    Impl& m = *m_impl;
    m.device->waitIdle();
    m.features.shutdownAll(*m.device);
    for (auto& [id, v] : m.views) {
        RenderView::Impl& vi = v->impl();
        for (auto& [key, st] : vi.featureStates) {
            if (st) {
                st->release(*m.device);
                delete st;
            }
        }
        for (auto& [name, h] : vi.histories) {
            for (auto& t : h.textures) {
                if (t) m.device->destroy(t);
            }
        }
        if (vi.clusterBuffer) m.device->destroy(vi.clusterBuffer);
        for (PickRequest& p : vi.picks) {
            if (p.readback) m.device->destroy(p.readback);
        }
        v->graph().reset();
        v->graph().releaseResources(*m.device);
    }
    m.views.clear();
    for (PickRequest& p : m.finishedPicks) {
        if (p.readback) m.device->destroy(p.readback);
    }
    BuiltinPipelines& p = m.pipelines;
    for (auto& row : p.prepass)
        for (auto h : row) m.device->destroy(h);
    for (auto h : p.forward) m.device->destroy(h);
    for (auto h : {p.lightCull, p.hiz, p.tonemap, p.resample, p.pick}) m.device->destroy(h);
    for (auto& [f, h] : p.finalBlit) m.device->destroy(h);
    m.device->destroy(m.zeroSH);
    m.frameAlloc.release(*m.device);
    m.gpuDriven.reset();
    m.cache->setStreamingHook(nullptr);
    m.streamer.reset();
    m.cache.reset();
    m.scene.reset();
}

rhi::Device& Renderer::device() { return *m_impl->device; }
FeatureRegistry& Renderer::features() { return m_impl->features; }
GpuScene& Renderer::scene() { return *m_impl->scene; }
GpuResourceCache& Renderer::resources() { return *m_impl->cache; }
const RenderSettings& Renderer::settings() const { return m_impl->settings; }
const RenderStats& Renderer::stats() const { return m_impl->stats; }
void Renderer::invalidateEnvironment() { m_impl->environmentDirty = true; }

ViewId Renderer::createView(const ViewDesc& desc) {
    const ViewId id = m_impl->nextViewId++;
    m_impl->views.emplace(id, std::make_unique<RenderView>(id, desc));
    return id;
}

void Renderer::destroyView(ViewId id) {
    Impl& m = *m_impl;
    auto it = m.views.find(id);
    if (it == m.views.end()) return;
    m.device->waitIdle();
    RenderView::Impl& vi = it->second->impl();
    for (auto& [key, st] : vi.featureStates) {
        if (st) {
            st->release(*m.device);
            delete st;
        }
    }
    for (auto& [name, h] : vi.histories) {
        for (auto& t : h.textures) {
            if (t) m.device->destroy(t);
        }
    }
    if (vi.clusterBuffer) m.device->destroy(vi.clusterBuffer);
    for (PickRequest& p : vi.picks) {
        if (p.readback) m.device->destroy(p.readback);
    }
    m.gpuDriven->releaseView(id);
    it->second->graph().reset();
    it->second->graph().releaseResources(*m.device);
    m.views.erase(it);
}

RenderView* Renderer::view(ViewId id) {
    auto it = m_impl->views.find(id);
    return it != m_impl->views.end() ? it->second.get() : nullptr;
}

void Renderer::trimMemory() {
    m_impl->device->waitIdle();
    for (auto& [id, v] : m_impl->views) v->graph().releaseResources(*m_impl->device);
}

void Renderer::Impl::createPipelines() {
    rhi::Device& dev = *device;
    for (u32 variant = 0; variant < kVariantCount; ++variant) {
        for (u32 entity = 0; entity < 2; ++entity) {
            rhi::GraphicsPipelineDesc d;
            d.name = std::format("render.prepass.v{}{}", variant, entity ? ".id" : "");
            d.vertex = rhi::ShaderStageDesc::file("render/passes/mesh.vert");
            std::vector<rhi::ShaderDefine> defs;
            if (variant & kVariantAlphaTest) defs.push_back({"OX_ALPHA_TEST"});
            if (entity) defs.push_back({"OX_ENTITY_ID"});
            d.fragment = rhi::ShaderStageDesc::file("render/passes/depth_prepass.frag", defs);
            d.colorFormats = {formats::kNormals, formats::kVelocity};
            if (entity) {
                d.colorFormats.push_back(formats::kEntityId);
                // Integer target: blending off with neutral factors (MoltenVK warns about non-default factors).
                rhi::BlendState noBlend;
                noBlend.srcColor = noBlend.srcAlpha = VK_BLEND_FACTOR_ONE;
                noBlend.dstColor = noBlend.dstAlpha = VK_BLEND_FACTOR_ZERO;
                noBlend.writeMask = VK_COLOR_COMPONENT_R_BIT;
                d.blend = {rhi::BlendState::opaque(), rhi::BlendState::opaque(), noBlend};
            }
            d.depthFormat = formats::kDepth;
            d.depth = {true, true, VK_COMPARE_OP_GREATER_OR_EQUAL};
            d.raster.cullMode = (variant & kVariantDoubleSided) ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
            d.raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            pipelines.prepass[variant][entity] = dev.createGraphicsPipeline(d);
        }
        rhi::GraphicsPipelineDesc f;
        f.name = std::format("render.forward.v{}", variant);
        f.vertex = rhi::ShaderStageDesc::file("render/passes/mesh.vert");
        f.fragment = rhi::ShaderStageDesc::file("render/passes/forward.frag");
        f.colorFormats = {formats::kSceneColor};
        f.depthFormat = formats::kDepth;
        f.depth = {true, false, VK_COMPARE_OP_EQUAL};
        f.raster.cullMode = (variant & kVariantDoubleSided) ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
        f.raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        pipelines.forward[variant] = dev.createGraphicsPipeline(f);
    }
    pipelines.lightCull = createComputePipeline(dev, "render.lightCull", "render/passes/light_cull.comp");
    pipelines.hiz = createComputePipeline(dev, "render.hiz", "render/passes/hiz.comp");
    pipelines.pick = createComputePipeline(dev, "render.pick", "render/editor/pick.comp");
    pipelines.tonemap = createFullscreenPipeline(dev, "render.tonemap", "render/passes/tonemap.frag", {formats::kSceneColorLDR});
    pipelines.resample = createFullscreenPipeline(dev, "render.resample", "render/passes/resample.frag", {formats::kSceneColor});
}

rhi::PipelineHandle Renderer::Impl::finalPipeline(VkFormat format) {
    auto it = pipelines.finalBlit.find(format);
    if (it != pipelines.finalBlit.end()) return it->second;
    std::vector<rhi::ShaderDefine> defs;
    if (isSrgb(format)) defs.push_back({"OX_SRGB_TARGET"});
    const rhi::PipelineHandle p =
        createFullscreenPipeline(*device, std::format("render.final.{}", u32(format)), "render/passes/final.frag", {format}, {},
                                 defs);
    pipelines.finalBlit.emplace(format, p);
    return p;
}

void Renderer::beginFrame(const RenderSnapshot& snapshot) {
    OX_PROFILE_ZONE();
    Impl& m = *m_impl;
    if (m.inFrame) endFrame();
    m.inFrame = true;
    m.frameStart = std::chrono::steady_clock::now();
    m.snapshot = &snapshot;
    m.settings = RenderSettings::fromCVars();
    if (!m.device->caps().rayTracingSupported()) m.settings.rayTracing = false;
    m.frameAlloc.beginFrame(m.device->frameIndex());
    m.building.resetCounters();
    m.building.frame = m.frameCounter;
    m.gpuDriven->beginFrame(m.building);
    m.cache->setTextureQuality(m.settings.anisotropy, m.settings.mipBias, m.settings.maxTextureSize);
    m.cache->update();
    glm::vec3 camPos(0.0f);
    if (const i32 c = snapshot.primaryCamera(); c >= 0) camPos = glm::vec3(snapshot.cameras[usize(c)].world[3]);
    m.scene->updateInstances(snapshot, *m.cache, m.settings.drawDistance, camPos);
    m.sceneUploaded = false;
    m.collectPicks();
}

void Renderer::render(const RenderSnapshot& snapshot, std::span<const ViewRenderRequest> views) {
    beginFrame(snapshot);
    for (const ViewRenderRequest& v : views) renderView(v);
    endFrame();
}

void Renderer::endFrame() {
    Impl& m = *m_impl;
    if (!m.inFrame) return;
    m.inFrame = false;
    RenderStats& s = m.building;
    // GPU timings of the last retired frame.
    s.passes.clear();
    s.gpuFrameMs = 0.0;
    const auto& timings = m.device->gpuTimings();
    f64 wallEnd = 0.0;
    for (const rhi::GpuTiming& t : timings) {
        const bool async = t.queue == u8(rhi::QueueType::Compute);
        auto it = std::find_if(s.passes.begin(), s.passes.end(), [&](const PassTiming& p) { return p.name == t.name; });
        if (it == s.passes.end()) s.passes.push_back({t.name, t.milliseconds, t.startMs, async});
        else it->gpuMs += t.milliseconds;
        if (t.depth == 0) s.gpuFrameMs += t.milliseconds;
        wallEnd = std::max(wallEnd, t.startMs + t.milliseconds);
        if (t.depth != 0 || !async) continue;
        // Async compute: time overlapped by graphics-queue scopes.
        s.asyncComputeMs += t.milliseconds;
        f64 overlap = 0.0;
        for (const rhi::GpuTiming& g : timings) {
            if (g.depth != 0 || g.queue != u8(rhi::QueueType::Graphics)) continue;
            overlap += std::max(0.0, std::min(t.startMs + t.milliseconds, g.startMs + g.milliseconds) - std::max(t.startMs, g.startMs));
        }
        s.asyncOverlapMs += std::min(overlap, t.milliseconds);
    }
    s.gpuFrameWallMs = wallEnd;
    s.instances = m.scene->liveInstanceCount();
    if (s.gpuDriven && s.gpuCulling.valid) {
        // Indirect draws: visibility and triangles come from the GPU counters (a few frames late).
        s.visibleInstances += s.gpuCulling.instancesVisible;
        s.triangles += s.gpuCulling.triangles + s.gpuCulling.meshletTriangles + s.gpuCulling.shadowTriangles;
    }
    const rhi::GpuMemoryStats mem = m.device->memoryStats();
    s.vramUsageBytes = mem.totalUsageBytes;
    s.vramBudgetBytes = mem.totalBudgetBytes;
    s.textures = mem.textureCount;
    s.buffers = mem.bufferCount;
    const GpuScene::Stats gs = m.scene->stats();
    s.geometryBytes = gs.positionBytes + gs.attributeBytes + gs.indexBytes;
    s.pendingAssetLoads = m.cache->pendingLoads();
    m.streamer->update(m.streamingCamera, s.streaming);
    s.pipelinesCompiling = m.device->pendingPipelineCompiles();
    s.vram.push_back({"geometry", s.geometryBytes});
    s.vram.push_back({"textures", m.cache->textureBytes()});
    s.vram.push_back({"gpu-driven buffers", m.gpuDriven->memoryBytes()});
    u64 transient = 0;
    for (auto& [id, v] : m.views) transient += v->graph().plan().transientBytesAliased;
    s.vram.push_back({"render targets (transient)", transient});
    s.cpuRenderMs = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - m.frameStart).count();
    m.stats = s;
    ++m.frameCounter;
    m.snapshot = nullptr;
}

void Renderer::renderView(const ViewRenderRequest& request) {
    OX_PROFILE_ZONE();
    Impl& m = *m_impl;
    OX_ASSERT(m.inFrame, "Renderer::renderView outside beginFrame/endFrame");
    RenderView* view = this->view(request.view);
    if (!view) {
        OX_LOG_ERROR("render", "renderView: unknown view {}", request.view);
        return;
    }
    rhi::Device& dev = *m.device;
    FrameState fs;
    fs.r = &m;
    fs.view = view;
    fs.snapshot = m.snapshot;
    fs.settings = request.settingsOverride ? *request.settingsOverride : m.settings;
    if (!dev.caps().rayTracingSupported()) fs.settings.rayTracing = false;
    fs.stats = &m.building;
    fs.firstViewOfFrame = !m.sceneUploaded;
    fs.allowParallelRecording = request.recordInto == nullptr;
    RenderView::Impl& cache = view->impl();
    auto swapCached = [&] {
        std::swap(fs.drawLists, cache.cachedLists[0]);
        std::swap(fs.gpuEarly, cache.cachedLists[1]);
        std::swap(fs.gpuLate, cache.cachedLists[2]);
        std::swap(fs.lights, cache.cachedLights);
        std::swap(fs.shadows, cache.cachedShadows);
    };
    swapCached();
    fs.lights.clear();
    fs.shadows.clear();

    rhi::RenderGraph& graph = view->graph();
    graph.reset();
    rhi::TextureHandle targetTex = request.target.swapchain ? request.target.swapchain->currentTexture() : request.target.texture;
    if (!targetTex) {
        OX_LOG_ERROR("render", "renderView: no target");
        swapCached();
        return;
    }
    const rhi::TextureDesc& td = dev.desc(targetTex);
    rhi::RGTexture output;
    if (request.target.swapchain) {
        output = graph.importTexture(targetTex, td, {rhi::Access::Undefined, rhi::Access::Present});
    } else {
        output = graph.importTexture(dev, targetTex, request.target.finalAccess);
    }
    fs.outputFormat = td.format;
    const u64 compilesBefore = graph.compileCount();
    m.buildView(fs, request, output, {td.width, td.height});
    m.commit(fs);
    if (request.recordInto) {
        graph.execute(*request.recordInto);
    } else {
        rhi::RGExecuteOptions eo;
        eo.swapchain = request.target.swapchain;
        eo.timestamps = fs.settings.gpuTimings;
        graph.execute(dev, eo);
    }
    m.sceneUploaded = true;

    // Pick requests recorded this frame complete with this submission (external command lists: resolved at the
    // next beginFrame, after the caller submitted).
    for (PickRequest& p : view->impl().picks) {
        if (p.recorded && p.done.value == 0 && !request.recordInto) p.done = dev.lastSubmitted(rhi::QueueType::Graphics);
        if (p.recorded && p.done.value == 0 && request.recordInto) p.recordedFrame = m.frameCounter;
    }
    m.building.views += 1;
    m.building.renderGraphPasses += graph.passCount();
    m.building.renderGraphCompiles += u32(graph.compileCount() - compilesBefore);
    m.building.featuresEnabled = u32(fs.features.size());
    m.building.lights += u32(fs.lights.size());
    swapCached();
    view->m_frameIndex += 1;
    view->m_cameraCut = false;
}

void Renderer::Impl::runFeatures(FrameState& fs, InjectionPoint point) {
    for (IRenderFeature* f : FeatureRegistry::at(fs.features, point)) {
        FeatureContext ctx(fs, f, point);
        f->setup(ctx);
    }
}

void Renderer::Impl::buildView(FrameState& fs, const ViewRenderRequest& request, rhi::RGTexture output,
                               Extent2D outputExtent) {
    RenderView& view = *fs.view;
    rhi::Device& dev = *device;
    rhi::RenderGraph& graph = view.graph();
    const RenderSettings& st = fs.settings;
    const RenderSnapshot& snap = *fs.snapshot;

    // --- features + view setup ---
    for (IRenderFeature* f : features.resolve(st, dev.caps())) {
        FeatureInitContext ic{dev, *self};
        if (features.ensureInitialized(*f, ic)) fs.features.push_back(f);
    }
    ViewSetup setup{st, dev.caps(), view, outputExtent};
    setup.screenPercentage = st.screenPercentage;
    for (IRenderFeature* f : fs.features) f->prepareView(setup);
    const f32 scale = std::clamp(setup.screenPercentage, 10.0f, 400.0f) / 100.0f;
    const Extent2D re{std::max(1u, u32(std::lround(f32(outputExtent.width) * scale))),
                      std::max(1u, u32(std::lround(f32(outputExtent.height) * scale)))};

    // --- camera, jitter, matrices ---
    if (view.m_cameraCutRequested || view.m_frameIndex == 0 || view.m_renderExtent != re) view.m_cameraCut = true;
    view.m_cameraCutRequested = false;
    view.m_camera = request.camera;
    view.m_outputExtent = outputExtent;
    view.m_renderExtent = re;
    view.m_prevJitter = view.m_jitter;
    view.m_jitter = haltonJitter(view.m_frameIndex, setup.jitterPhases);
    const glm::vec2 jndc = view.jitterNdc();
    const glm::mat4 prevUnjittered = view.m_unjitteredViewProj;
    const glm::mat4 prevJittered = view.m_viewProj;
    view.m_view = request.camera.viewMatrix();
    view.m_unjitteredProj = request.camera.projectionMatrix(outputExtent.aspect());
    view.m_proj = glm::translate(glm::mat4(1.0f), glm::vec3(jndc, 0.0f)) * view.m_unjitteredProj;
    view.m_viewProj = view.m_proj * view.m_view;
    view.m_unjitteredViewProj = view.m_unjitteredProj * view.m_view;
    view.m_prevUnjitteredViewProj = view.m_cameraCut ? view.m_unjitteredViewProj : prevUnjittered;
    view.m_prevViewProj = view.m_cameraCut ? view.m_viewProj : prevJittered;

    // --- lights ---
    const glm::vec3 camPos = request.camera.position();
    thread_local std::vector<i32> gpuIndex; // scratch, capacity kept
    gpuIndex.assign(snap.lights.size(), -1);
    for (usize i = 0; i < snap.lights.size(); ++i) {
        const SnapshotLight& sl = snap.lights[i];
        if (sl.light.type != LightType::Directional) continue;
        GpuLight g;
        g.type = u32(GpuLightType::Directional);
        g.color = sl.light.color * sl.light.intensity;
        g.direction = sl.direction;
        g.position = sl.position;
        g.sourceRadius = glm::radians(sl.light.sourceRadius);
        g.entityId = sl.entityId;
        gpuIndex[i] = i32(fs.lights.size());
        fs.lights.push_back(g);
    }
    fs.directionalCount = u32(fs.lights.size());
    thread_local std::vector<u32> locals;
    locals.clear();
    for (usize i = 0; i < snap.lights.size(); ++i) {
        const LightType t = snap.lights[i].light.type;
        if (t == LightType::Point || t == LightType::Spot) locals.push_back(u32(i));
    }
    if (locals.size() > usize(std::max(st.maxLights, 0))) {
        std::sort(locals.begin(), locals.end(), [&](u32 a, u32 b) {
            return glm::length(snap.lights[a].position - camPos) - snap.lights[a].light.range <
                   glm::length(snap.lights[b].position - camPos) - snap.lights[b].light.range;
        });
        locals.resize(usize(std::max(st.maxLights, 0)));
    }
    for (u32 i : locals) {
        const SnapshotLight& sl = snap.lights[i];
        GpuLight g;
        g.type = u32(sl.light.type == LightType::Spot ? GpuLightType::Spot : GpuLightType::Point);
        g.position = sl.position;
        g.range = std::max(sl.light.range, 0.01f);
        g.color = sl.light.color * (sl.light.intensity / (4.0f * kPi)); // lumens → candela
        g.direction = sl.direction;
        g.sourceRadius = sl.light.sourceRadius;
        const f32 cosOuter = std::cos(glm::radians(std::clamp(sl.light.outerConeAngle, 0.1f, 89.9f)));
        const f32 cosInner = std::cos(glm::radians(std::clamp(sl.light.innerConeAngle, 0.0f, sl.light.outerConeAngle)));
        g.spotScale = 1.0f / std::max(cosInner - cosOuter, 1e-4f);
        g.spotOffset = -cosOuter * g.spotScale;
        g.entityId = sl.entityId;
        gpuIndex[i] = i32(fs.lights.size());
        fs.lights.push_back(g);
    }
    i32 sun = -1;
    if (snap.environment && snap.environment->sunLight >= 0 && usize(snap.environment->sunLight) < gpuIndex.size()) {
        sun = gpuIndex[usize(snap.environment->sunLight)];
    }
    if (sun < 0 && fs.directionalCount > 0) sun = 0;

    // --- view constants ---
    GpuViewConstants& c = fs.constants;
    c.view = view.m_view;
    c.proj = view.m_proj;
    c.viewProj = view.m_viewProj;
    c.invView = glm::inverse(view.m_view);
    c.invProj = glm::inverse(view.m_proj);
    c.invViewProj = glm::inverse(view.m_viewProj);
    c.unjitteredViewProj = view.m_unjitteredViewProj;
    c.prevUnjitteredViewProj = view.m_prevUnjitteredViewProj;
    c.prevViewProj = view.m_prevViewProj;
    c.cameraPosition = glm::vec4(camPos, f32(snap.time));
    c.renderSize = {f32(re.width), f32(re.height), 1.0f / f32(re.width), 1.0f / f32(re.height)};
    c.outputSize = {f32(outputExtent.width), f32(outputExtent.height), 1.0f / f32(std::max(outputExtent.width, 1u)),
                    1.0f / f32(std::max(outputExtent.height, 1u))};
    const glm::vec2 prevJndc =
        view.m_renderExtent.width ? view.m_prevJitter * 2.0f / glm::vec2(f32(re.width), f32(re.height)) : glm::vec2(0.0f);
    c.jitter = {jndc, prevJndc};
    c.nearPlane = request.camera.nearPlane;
    c.farPlane = request.camera.farPlane > 0.0f ? request.camera.farPlane : 0.0f;
    const f32 ev = (st.exposureMode == ExposureMode::Manual ? st.manualEV100 : request.camera.ev100) - st.exposureCompensation;
    c.ev100 = ev;
    c.exposure = 1.0f / (1.2f * std::exp2(ev));
    c.preExposure = c.exposure;
    c.deltaTime = snap.deltaTime;
    c.frameIndex = u32(view.m_frameIndex);
    c.debugView = u32(st.debugView);
    c.flags = (request.camera.projection == CameraParams::Projection::Orthographic ? 1u : 0u) |
              (view.desc().flags.editor ? 2u : 0u);
    c.lodBias = st.lodBias;
    c.mipBias = setup.mipBias;
    ClusterGrid grid;
    grid.x = kClusterX;
    grid.y = kClusterY;
    grid.z = kClusterZ;
    grid.maxLightsPerCluster = u32(st.clusterMaxLights);
    grid.nearPlane = std::max(request.camera.nearPlane, 0.01f);
    grid.farPlane = std::max(request.camera.farPlane > 0.0f ? std::min(request.camera.farPlane, st.clusterMaxDistance)
                                                            : st.clusterMaxDistance,
                             grid.nearPlane * 2.0f);
    c.clusterGrid = {grid.x, grid.y, grid.z, grid.maxLightsPerCluster};
    c.clusterDepth = {grid.sliceScale(), grid.sliceBias(), grid.nearPlane, grid.farPlane};
    RenderView::Impl& vi = view.impl();
    if (!vi.clusterBuffer || vi.clusterBufferSize != grid.bufferSize()) {
        if (vi.clusterBuffer) dev.destroy(vi.clusterBuffer);
        vi.clusterBufferSize = grid.bufferSize();
        vi.clusterBuffer = dev.createBuffer({vi.clusterBufferSize, rhi::BufferUsage::Storage, rhi::MemoryUsage::GpuOnly,
                                             "render.lightClusters"});
    }
    c.clusterBuffer = dev.address(vi.clusterBuffer);
    const u32 localCount = u32(fs.lights.size()) - fs.directionalCount;
    c.lightClusterCount = localCount > 0 ? grid.clusterCount() : 0;
    c.sunLight = sun;
    if (sun >= 0) c.sunAngularRadius = fs.lights[usize(sun)].sourceRadius;
    c.irradianceSH = dev.address(zeroSH);
    c.iblIntensity = st.iblIntensity * (snap.environment ? snap.environment->environment.ambientIntensity : 1.0f);
    c.skyIntensity = snap.environment ? snap.environment->environment.skyIntensity : 1.0f;
    if (snap.environment && snap.environment->environment.fogEnabled) {
        const EnvironmentComponent& env = snap.environment->environment;
        c.fogColor = glm::vec4(env.fogColor, 1.0f);
        c.fogParams = {env.fogDensity, env.fogHeightFalloff, env.fogStartDistance, 0.0f};
    }
    const DefaultTextures& defs = cache->defaults();
    c.whiteTexture = defs.whiteIndex;
    c.blackTexture = defs.blackIndex;
    c.flatNormalTexture = defs.flatNormalIndex;

    if (fs.firstViewOfFrame) streamingCamera = FeatureContext(fs, nullptr, InjectionPoint::PreDepth).lodSelection();
    fs.constantsAlloc = frameAlloc.allocate(sizeof(GpuViewConstants), 16);
    fs.headerAlloc = frameAlloc.allocate(sizeof(GpuSceneHeader), 16);
    scene->fillHeader(fs.header);

    // --- draw lists (CPU culling; lazily on first FeatureContext::drawLists() when GPU-driven) ---
    fs.drawDistance = st.drawDistance > 0.0f ? st.drawDistance : (request.camera.farPlane > 0.0f ? request.camera.farPlane : 0.0f);
    if (gpuDriven->active()) fs.cpuDrawListsBuilt = false;
    else buildCpuDrawLists(fs);

    FrameResources& R = fs.resources;
    R.setTexture(res::kOutput, output);
    const VkDeviceAddress viewAddr = fs.constantsAlloc.address;
    const VkDeviceAddress sceneAddr = fs.headerAlloc.address;
    FeatureContext core(fs, nullptr, InjectionPoint::PreDepth);

    // --- scene upload (first view of the frame) ---
    if (fs.firstViewOfFrame && scene->hasPendingUploads()) {
        graph.addPass("SceneUpload", rhi::PassType::Transfer).sideEffect().execute([this](rhi::PassContext& ctx) {
            scene->recordUploads(ctx.cmd, [this](u64 size) { return frameAlloc.allocate(size, 16); });
        });
    }

    gpuDriven->setupView(fs); // "GpuCull" pass + fs.gpuEarly / fs.gpuLate (r.GpuDriven)
    fs.stats->gpuDriven = fs.stats->gpuDriven || fs.gpuDriven;

    runFeatures(fs, InjectionPoint::PreDepth);

    // --- depth prepass ---
    const bool editor = view.desc().flags.editor;
    const rhi::RGTexture depth = graph.createTexture(targetDesc(formats::kDepth, re, "Depth"));
    const rhi::RGTexture normals = graph.createTexture(targetDesc(formats::kNormals, re, "Normals"));
    const rhi::RGTexture velocity = graph.createTexture(targetDesc(formats::kVelocity, re, "Velocity"));
    rhi::RGTexture entity;
    {
        rhi::PassBuilder pb = graph.addPass("DepthPrepass");
        pb.color(normals, VK_ATTACHMENT_LOAD_OP_CLEAR, rhi::ClearColor::rgba(0, 0, 0, 1))
            .color(velocity, VK_ATTACHMENT_LOAD_OP_CLEAR)
            .depth(depth, VK_ATTACHMENT_LOAD_OP_CLEAR, {0.0f, 0});
        if (editor) {
            entity = graph.createTexture(targetDesc(formats::kEntityId, re, "EntityID"));
            pb.color(entity, VK_ATTACHMENT_LOAD_OP_CLEAR);
        }
        if (!fs.gpuDriven && wantsParallelRecording(fs, {&fs.drawLists[DrawBucket::Opaque], &fs.drawLists[DrawBucket::Masked]})) {
            pb.secondaryCommandLists();
        }
        pb.execute([&fs, viewAddr, sceneAddr, editor, this](rhi::PassContext& ctx) {
            FeatureContext fc(fs, nullptr, InjectionPoint::PreDepth);
            const rhi::PipelineHandle pipes[4] = {pipelines.prepass[0][editor], pipelines.prepass[1][editor],
                                                  pipelines.prepass[2][editor], pipelines.prepass[3][editor]};
            if (ctx.secondaryRendering) {
                DrawPush pc;
                pc.view = viewAddr;
                pc.scene = sceneAddr;
                recordParallel(fs, ctx, {&fs.drawLists[DrawBucket::Opaque], &fs.drawLists[DrawBucket::Masked]}, pipes, &pc, sizeof(pc));
                return;
            }
            const ViewDrawLists& lists = fs.gpuDriven ? fs.gpuEarly : fs.drawLists;
            for (DrawBucket b : {DrawBucket::Opaque, DrawBucket::Masked}) {
                const DrawList& list = lists[b];
                DrawPush pc;
                pc.view = viewAddr;
                pc.scene = sceneAddr;
                pc.drawIds = list.instanceIds;
                fc.drawBatches(ctx.cmd, list, pipes, &pc, sizeof(pc));
            }
            DrawPush mp;
            mp.view = viewAddr;
            mp.scene = sceneAddr;
            gpuDriven->drawMeshlets(fs, ctx.cmd, false, false, editor, &mp, sizeof(mp));
        });
    }
    // Two-phase occlusion: HiZ of the phase-1 depth, cull the rest, draw the newly visible instances.
    if (gpuDriven->declareLate(fs, depth)) {
        rhi::PassBuilder pb = graph.addPass("DepthPrepass.Late");
        pb.color(normals, VK_ATTACHMENT_LOAD_OP_LOAD).color(velocity, VK_ATTACHMENT_LOAD_OP_LOAD).depth(depth, VK_ATTACHMENT_LOAD_OP_LOAD);
        if (entity.valid()) pb.color(entity, VK_ATTACHMENT_LOAD_OP_LOAD);
        pb.execute([&fs, viewAddr, sceneAddr, editor, this](rhi::PassContext& ctx) {
            FeatureContext fc(fs, nullptr, InjectionPoint::PreDepth);
            const rhi::PipelineHandle pipes[4] = {pipelines.prepass[0][editor], pipelines.prepass[1][editor],
                                                  pipelines.prepass[2][editor], pipelines.prepass[3][editor]};
            for (DrawBucket b : {DrawBucket::Opaque, DrawBucket::Masked}) {
                DrawPush pc;
                pc.view = viewAddr;
                pc.scene = sceneAddr;
                pc.drawIds = fs.gpuLate[b].instanceIds;
                fc.drawBatches(ctx.cmd, fs.gpuLate[b], pipes, &pc, sizeof(pc));
            }
            DrawPush mp;
            mp.view = viewAddr;
            mp.scene = sceneAddr;
            gpuDriven->drawMeshlets(fs, ctx.cmd, true, false, editor, &mp, sizeof(mp));
        });
    }
    R.setTexture(res::kDepth, depth);
    R.setTexture(res::kNormals, normals);
    R.setTexture(res::kVelocity, velocity);
    if (entity.valid()) R.setTexture(res::kEntityId, entity);

    // --- HiZ (culled automatically when nobody reads it) ---
    {
        const u32 mips = rhi::fullMipCount(re.width, re.height);
        const rhi::RGTexture hiz = graph.createTexture(targetDesc(formats::kHiZ, re, "HiZ", mips));
        graph.addPass("HiZ", rhi::PassType::Compute)
            .queue(asyncComputeHint(dev.caps()))
            .read(depth, rhi::Access::SampledCompute)
            .overwrite(hiz, rhi::Access::StorageWriteCompute)
            .execute([this, depth, hiz, re, mips](rhi::PassContext& ctx) {
                struct {
                    u32 src, dst;
                    u32 srcSize[2], dstSize[2];
                    u32 fromDepth;
                } pc{};
                ctx.cmd.bindPipeline(pipelines.hiz);
                u32 w = re.width, h = re.height;
                for (u32 mip = 0; mip < mips; ++mip) {
                    const u32 dw = mip == 0 ? w : std::max(w >> 1, 1u), dh = mip == 0 ? h : std::max(h >> 1, 1u);
                    pc.src = mip == 0 ? ctx.sampledIndex(depth) : ctx.storageIndex(hiz, mip - 1);
                    pc.dst = ctx.storageIndex(hiz, mip);
                    pc.srcSize[0] = w, pc.srcSize[1] = h, pc.dstSize[0] = dw, pc.dstSize[1] = dh;
                    pc.fromDepth = mip == 0 ? 1u : 0u;
                    ctx.cmd.pushConstants(pc);
                    ctx.cmd.dispatch((dw + 7) / 8, (dh + 7) / 8);
                    ctx.cmd.memoryBarrier(rhi::Access::StorageWriteCompute, rhi::Access::StorageWriteCompute);
                    w = dw, h = dh;
                }
            });
        R.setTexture(res::kHiZ, hiz);
    }

    runFeatures(fs, InjectionPoint::AfterDepth);

    // --- light culling ---
    const rhi::RGBuffer clusters = graph.importBuffer(
        vi.clusterBuffer, {vi.clusterBufferSize, rhi::BufferUsage::Storage, rhi::MemoryUsage::GpuOnly, "LightClusters"},
        {rhi::Access::General, rhi::Access::Undefined});
    graph.addPass("LightCulling", rhi::PassType::Compute)
        .queue(asyncComputeHint(dev.caps()))
        .overwrite(clusters, rhi::Access::StorageWriteCompute)
        .execute([this, viewAddr, sceneAddr, grid](rhi::PassContext& ctx) {
            ctx.cmd.bindPipeline(pipelines.lightCull);
            ctx.cmd.pushConstants(ViewScenePush{viewAddr, sceneAddr, 0});
            ctx.cmd.dispatch((grid.clusterCount() + 63) / 64);
        });
    R.setBuffer(res::kLightClusters, clusters);

    runFeatures(fs, InjectionPoint::Shadows);
    runFeatures(fs, InjectionPoint::Lighting);

    // --- forward opaque ---
    const rhi::RGTexture hdr = graph.createTexture(targetDesc(formats::kSceneColor, re, "SceneColorHDR"));
    {
        rhi::PassBuilder pb = graph.addPass("ForwardOpaque");
        pb.color(hdr, VK_ATTACHMENT_LOAD_OP_CLEAR, rhi::ClearColor::rgba(0, 0, 0, 1))
            .depth(depth, VK_ATTACHMENT_LOAD_OP_LOAD, {}, true)
            .read(clusters, rhi::Access::StorageReadGraphics);
        const std::string_view inputNames[4] = {res::kShadowMask, res::kAO, res::kReflectionsSpecular, res::kIndirectDiffuse};
        std::array<rhi::RGTexture, 4> inputs{};
        for (u32 i = 0; i < 4; ++i) {
            inputs[i] = R.texture(inputNames[i]);
            if (inputs[i].valid()) pb.read(inputs[i], rhi::Access::SampledFragment);
        }
        for (std::string_view n : {res::kShadowCascades, res::kShadowAtlas, res::kPointShadows}) {
            if (rhi::RGTexture t = R.texture(n); t.valid()) pb.read(t, rhi::Access::SampledFragment);
        }
        if (!fs.gpuDriven && wantsParallelRecording(fs, {&fs.drawLists[DrawBucket::Opaque], &fs.drawLists[DrawBucket::Masked]})) {
            pb.secondaryCommandLists();
        }
        pb.execute([&fs, viewAddr, sceneAddr, inputs, this](rhi::PassContext& ctx) {
            FeatureContext fc(fs, nullptr, InjectionPoint::Lighting);
            DrawPush pc;
            pc.view = viewAddr;
            pc.scene = sceneAddr;
            for (u32 i = 0; i < 4; ++i) pc.inputs[i] = inputs[i].valid() ? ctx.sampledIndex(inputs[i]) : kInvalidIndex;
            if (ctx.secondaryRendering) {
                recordParallel(fs, ctx, {&fs.drawLists[DrawBucket::Opaque], &fs.drawLists[DrawBucket::Masked]},
                               pipelines.forward, &pc, sizeof(pc));
                return;
            }
            for (const ViewDrawLists* lists : {fs.gpuDriven ? &fs.gpuEarly : &fs.drawLists, fs.gpuLateActive ? &fs.gpuLate : nullptr}) {
                if (!lists) continue;
                for (DrawBucket b : {DrawBucket::Opaque, DrawBucket::Masked}) {
                    const DrawList& list = (*lists)[b];
                    pc.drawIds = list.instanceIds;
                    fc.drawBatches(ctx.cmd, list, pipelines.forward, &pc, sizeof(pc));
                }
            }
            for (bool late : {false, true}) gpuDriven->drawMeshlets(fs, ctx.cmd, late, true, false, &pc, sizeof(pc));
        });
    }
    R.setTexture(res::kSceneColorHDR, hdr);

    runFeatures(fs, InjectionPoint::AfterOpaque);
    runFeatures(fs, InjectionPoint::Translucency);
    runFeatures(fs, InjectionPoint::BeforePostProcess);
    runFeatures(fs, InjectionPoint::PostProcess);

    // --- upscale (single slot) ---
    const auto upscalers = FeatureRegistry::at(fs.features, InjectionPoint::Upscale);
    if (!upscalers.empty()) {
        runFeatures(fs, InjectionPoint::Upscale);
    } else if (re != outputExtent) {
        const rhi::RGTexture src = R.texture(res::kSceneColorHDR);
        const rhi::RGTexture up = graph.createTexture(targetDesc(formats::kSceneColor, outputExtent, "SceneColorHDROutput"));
        graph.addPass("Resample")
            .read(src, rhi::Access::SampledFragment)
            .color(up, VK_ATTACHMENT_LOAD_OP_DONT_CARE)
            .execute([this, src](rhi::PassContext& ctx) {
                const u32 idx = ctx.sampledIndex(src);
                drawFullscreen(ctx.cmd, pipelines.resample, &idx, sizeof(idx));
            });
        R.setTexture(res::kSceneColorHDR, up);
    }
    runFeatures(fs, InjectionPoint::AfterUpscale);

    // --- tonemap ---
    const rhi::RGTexture ldr = graph.createTexture(targetDesc(formats::kSceneColorLDR, outputExtent, "SceneColorLDR"));
    {
        const rhi::RGTexture src = R.texture(res::kSceneColorHDR);
        const rhi::RGTexture exposureTex = R.texture("Exposure");
        rhi::PassBuilder pb = graph.addPass("Tonemap");
        pb.read(src, rhi::Access::SampledFragment).color(ldr, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
        if (exposureTex.valid()) pb.read(exposureTex, rhi::Access::SampledFragment);
        const u32 dv = u32(st.debugView);
        const u32 bypass = (dv >= 1 && dv <= 7) || dv == 9 ? 1u : 0u;
        const u32 op = u32(st.tonemapper);
        pb.execute([this, src, exposureTex, viewAddr, bypass, op](rhi::PassContext& ctx) {
            struct {
                u64 view;
                u32 hdr, tonemapper, exposureTex, debugBypass;
            } pc{viewAddr, ctx.sampledIndex(src), op, exposureTex.valid() ? ctx.sampledIndex(exposureTex) : kInvalidIndex,
                 bypass};
            drawFullscreen(ctx.cmd, pipelines.tonemap, &pc, sizeof(pc));
        });
    }
    R.setTexture(res::kSceneColorLDR, ldr);

    runFeatures(fs, InjectionPoint::Overlay);
    runFeatures(fs, InjectionPoint::Debug);

    // --- final copy into the target ---
    {
        const rhi::RGTexture src = R.texture(res::kSceneColorLDR);
        const rhi::PipelineHandle finalPipe = finalPipeline(fs.outputFormat);
        graph.addPass("Final")
            .read(src, rhi::Access::SampledFragment)
            .color(output, VK_ATTACHMENT_LOAD_OP_DONT_CARE)
            .execute([src, finalPipe](rhi::PassContext& ctx) {
                const u32 idx = ctx.sampledIndex(src);
                drawFullscreen(ctx.cmd, finalPipe, &idx, sizeof(idx));
            });
    }

    // --- picking readback ---
    if (entity.valid()) {
        for (PickRequest& p : vi.picks) {
            if (p.recorded) continue;
            const u32 rx = std::min(u32(f32(p.x) * f32(re.width) / f32(outputExtent.width)), re.width - 1);
            const u32 ry = std::min(u32(f32(p.y) * f32(re.height) / f32(outputExtent.height)), re.height - 1);
            const u32 rw = std::clamp(u32(std::ceil(f32(p.w) * f32(re.width) / f32(outputExtent.width))), 1u, re.width - rx);
            const u32 rh = std::clamp(u32(std::ceil(f32(p.h) * f32(re.height) / f32(outputExtent.height))), 1u, re.height - ry);
            p.result.x = rx, p.result.y = ry, p.result.width = rw, p.result.height = rh;
            p.readback = dev.createBuffer({u64(rw) * rh * 4, rhi::BufferUsage::Storage, rhi::MemoryUsage::Readback,
                                           "render.pickReadback"});
            const VkDeviceAddress dst = dev.address(p.readback);
            p.recorded = true;
            graph.addPass("Pick", rhi::PassType::Compute)
                .read(entity, rhi::Access::SampledCompute)
                .sideEffect()
                .execute([this, entity, dst, rx, ry, rw, rh](rhi::PassContext& ctx) {
                    struct {
                        u64 dst;
                        u32 tex, x, y, w, h;
                    } pc{dst, ctx.sampledIndex(entity), rx, ry, rw, rh};
                    ctx.cmd.bindPipeline(pipelines.pick);
                    ctx.cmd.pushConstants(pc);
                    ctx.cmd.dispatch((rw + 7) / 8, (rh + 7) / 8);
                    ctx.cmd.memoryBarrier(rhi::Access::StorageWriteCompute, rhi::Access::HostRead);
                });
        }
    }
    (void)core;
}

bool Renderer::Impl::wantsParallelRecording(const FrameState& fs, std::initializer_list<const DrawList*> lists) const {
    if (!desc.jobs || !fs.allowParallelRecording || !gpuDriven->settings().parallelRecording) return false;
    usize batches = 0;
    for (const DrawList* l : lists) batches += l->batches.size();
    return batches >= usize(std::max(gpuDriven->settings().parallelMinBatches, 1));
}

void Renderer::Impl::recordParallel(FrameState& fs, rhi::PassContext& ctx, std::initializer_list<const DrawList*> lists,
                                    const rhi::PipelineHandle* pipes, const void* push, u32 pushSize) {
    OX_PROFILE_ZONE();
    constexpr u32 kMaxChunks = 32;
    const DrawList* ls[8];
    u32 listCount = 0, total = 0;
    for (const DrawList* l : lists) {
        ls[listCount++] = l;
        total += u32(l->batches.size());
    }
    if (total == 0) return;
    const u32 perChunk = 64;
    const u32 chunks = std::clamp(total / perChunk, 1u, std::min(kMaxChunks, desc.jobs->threadCount() * 2));
    std::array<rhi::CommandList*, kMaxChunks> recorded{};
    std::array<u32, kMaxChunks> draws{};
    std::array<u64, kMaxChunks> tris{};
    const rhi::SecondaryRenderingInfo& info = *ctx.secondaryRendering;
    rhi::Device& dev = *device;
    const rhi::BufferHandle indices = scene->indexBuffer();
    u8 pc[rhi::kMaxPushConstantSize];
    std::memcpy(pc, push, pushSize);
    desc.jobs->parallelFor(chunks, 1, [&](u32 begin, u32 end, u32) {
        for (u32 c = begin; c < end; ++c) {
            const u32 thread = JobSystem::currentThreadIndex();
            OX_ASSERT(thread < rhi::Device::kMaxRecordingThreads, "recording thread {} has no command pool slot", thread);
            rhi::CommandList& cmd = dev.secondaryCommandList(thread, info);
            cmd.bindIndexBuffer(indices);
            const u32 first = u64(total) * c / chunks, last = u64(total) * (c + 1) / chunks;
            u32 global = 0;
            rhi::PipelineHandle bound;
            u8 local[rhi::kMaxPushConstantSize];
            std::memcpy(local, pc, pushSize);
            for (u32 li = 0; li < listCount; ++li) {
                const DrawList& l = *ls[li];
                const u32 n = u32(l.batches.size());
                if (global + n <= first || global >= last) {
                    global += n;
                    continue;
                }
                std::memcpy(local + 16, &l.instanceIds, sizeof(u64)); // drawIds of this list (DrawPush layout)
                bool pushed = false;
                for (u32 b = std::max(first, global) - global; b < n && global + b < last; ++b) {
                    const DrawBatch& db = l.batches[b];
                    const rhi::PipelineHandle p = pipes[std::min<u32>(db.variant, 3)];
                    if (!p) continue;
                    if (p != bound) {
                        cmd.bindPipeline(p);
                        bound = p;
                    }
                    if (!pushed) {
                        cmd.pushConstants(local, pushSize);
                        pushed = true;
                    }
                    cmd.drawIndexed(db.indexCount, db.instanceCount, db.firstIndex, db.vertexOffset, db.firstInstance);
                    draws[c] += 1;
                    tris[c] += u64(db.indexCount / 3) * db.instanceCount;
                }
                global += n;
            }
            cmd.end();
            recorded[c] = &cmd;
        }
    });
    ctx.cmd.executeSecondary(std::span<rhi::CommandList* const>(recorded.data(), chunks));
    for (u32 c = 0; c < chunks; ++c) {
        fs.stats->drawCalls += draws[c];
        fs.stats->triangles += tris[c];
    }
    fs.stats->parallelRecordedChunks += chunks;
}

void Renderer::Impl::buildCpuDrawLists(FrameState& fs) {
    OX_PROFILE_ZONE();
    fs.cpuDrawListsBuilt = true;
    RenderView& view = *fs.view;
    RenderView::Impl& vi = view.impl();
    const LodSelection lod = FeatureContext(fs, nullptr, InjectionPoint::PreDepth).lodSelection();
    scene->buildViewDrawLists(view.frustum(), glm::vec3(fs.constants.cameraPosition), fs.drawLists, vi.drawIds,
                              fs.drawDistance, fs.settings.frustumCulling, &lod);
    for (u32 b = 0; b < u32(DrawBucket::Count); ++b) {
        GpuAllocation a = frameAlloc.allocate(std::max<u64>(vi.drawIds[b].size(), 1) * 4, 16);
        if (!vi.drawIds[b].empty()) std::memcpy(a.cpu, vi.drawIds[b].data(), vi.drawIds[b].size() * 4);
        fs.drawLists.buckets[b].instanceIds = a.address;
        if (!fs.gpuDriven) fs.stats->visibleInstances += fs.drawLists.buckets[b].instanceCount;
    }
}

void Renderer::Impl::commit(FrameState& fs) {
    GpuSceneHeader& h = fs.header;
    const GpuAllocation lights = frameAlloc.allocate(std::max<u64>(fs.lights.size(), 1) * sizeof(GpuLight), 16);
    if (!fs.lights.empty()) std::memcpy(lights.cpu, fs.lights.data(), fs.lights.size() * sizeof(GpuLight));
    const GpuAllocation shadows = frameAlloc.allocate(std::max<u64>(fs.shadows.size(), 1) * sizeof(GpuShadow), 16);
    if (!fs.shadows.empty()) std::memcpy(shadows.cpu, fs.shadows.data(), fs.shadows.size() * sizeof(GpuShadow));
    h.lights = lights.address;
    h.shadows = shadows.address;
    h.lightCount = u32(fs.lights.size());
    h.directionalLightCount = fs.directionalCount;
    h.shadowCount = u32(fs.shadows.size());
    const auto& palettes = fs.snapshot->palettes;
    const GpuAllocation pal = frameAlloc.allocate(std::max<u64>(palettes.size(), 1) * sizeof(glm::mat4), 16);
    if (!palettes.empty()) std::memcpy(pal.cpu, palettes.data(), palettes.size() * sizeof(glm::mat4));
    h.palettes = pal.address;
    std::memcpy(fs.headerAlloc.cpu, &h, sizeof(h));
    std::memcpy(fs.constantsAlloc.cpu, &fs.constants, sizeof(fs.constants));
}

// --- picking ---

PickRequestId Renderer::requestPick(ViewId viewId, u32 x, u32 y, u32 width, u32 height) {
    RenderView* v = view(viewId);
    if (!v || !v->desc().flags.editor) return 0;
    PickRequest p;
    p.id = m_impl->nextPick++;
    p.x = x, p.y = y, p.w = std::max(width, 1u), p.h = std::max(height, 1u);
    v->impl().picks.push_back(p);
    return p.id;
}

void Renderer::Impl::collectPicks() {
    for (auto& [id, v] : views) {
        auto& picks = v->impl().picks;
        for (PickRequest& p : picks) {
            if (p.recorded && p.done.value == 0 && p.recordedFrame != ~0ull && p.recordedFrame < frameCounter) {
                p.done = device->lastSubmitted(rhi::QueueType::Graphics);
            }
        }
        for (auto it = picks.begin(); it != picks.end();) {
            if (it->recorded && it->done.value != 0 && device->isComplete(it->done)) {
                const u32 n = it->result.width * it->result.height;
                it->result.ids.resize(n);
                const void* src = device->mapped(it->readback);
                if (src) std::memcpy(it->result.ids.data(), src, n * 4);
                else {
                    std::vector<u8> bytes = device->readBuffer(it->readback);
                    std::memcpy(it->result.ids.data(), bytes.data(), std::min<usize>(bytes.size(), n * 4));
                }
                it->result.ready = true;
                device->destroy(it->readback);
                it->readback = {};
                finishedPicks.push_back(std::move(*it));
                it = picks.erase(it);
            } else {
                ++it;
            }
        }
    }
}

PickResult Renderer::takePickResult(PickRequestId id) {
    m_impl->collectPicks();
    auto& fp = m_impl->finishedPicks;
    for (auto it = fp.begin(); it != fp.end(); ++it) {
        if (it->id == id) {
            PickResult r = std::move(it->result);
            fp.erase(it);
            return r;
        }
    }
    return {};
}

u32 PickResult::dominant() const {
    std::unordered_map<u32, u32> counts;
    u32 best = 0, bestCount = 0;
    for (u32 id : ids) {
        if (id == 0) continue;
        const u32 c = ++counts[id];
        if (c > bestCount) best = id, bestCount = c;
    }
    return best;
}

std::vector<u32> PickResult::unique() const {
    std::vector<u32> out;
    for (u32 id : ids) {
        if (id != 0) out.push_back(id);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void registerBuiltinFeatures(Renderer& renderer) {
    FeatureRegistry& f = renderer.features();
    f.add(makeEnvironmentFeature());
    f.add(makeShadowsRasterFeature());
    f.add(makeSkyFeature());
    f.add(makeDebugLinesFeature());
    f.add(makeEditorOverlaysFeature());
    f.add(makeDebugViewsFeature());

    // Feature areas register here. Each team owns exactly one line below: replace your marker with a call
    // like `registerReflectionFeatures(f);` (declared in features/<area>/...); keep the other lines untouched.
    registerReflectionFeatures(f); // [feature-area: reflections-ao]
    volumetrics::registerVolumetricsFeatures(f); // [feature-area: volumetrics]
    registerTranslucencyFeatures(f); // [feature-area: translucency-water-particles]
    registerRayTracingFeatures(f); // [feature-area: raytracing]
    registerPostProcessFeatures(f); // [feature-area: postprocess-upscalers]
    registerGpuDrivenFeatures(f); // [feature-area: gpu-driven]
    registerWorldSkinningFeatures(f); // [feature-area: world-skinning]
    // [feature-area: ui] added by Oxwald::ui per renderer (ui::attachRenderer / ui::withUi); render does not link ui
}

} // namespace ox::render
