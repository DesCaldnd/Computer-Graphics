#include "renderer_impl.hpp"

#include "features/gpu_driven/gpu_driven.hpp"

#include <oxwald/core/log.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

namespace ox::render {

const char* injectionPointName(InjectionPoint p) {
    static constexpr const char* kNames[] = {"PreDepth",    "AfterDepth",        "Shadows",     "Lighting",
                                             "AfterOpaque", "Translucency",      "BeforePostProcess",
                                             "PostProcess", "Upscale",           "AfterUpscale", "Overlay", "Debug"};
    return u32(p) < std::size(kNames) ? kNames[u32(p)] : "?";
}

// --- factories ---

namespace {
std::mutex& factoryMutex() {
    static std::mutex m;
    return m;
}
std::vector<std::pair<std::string, FeatureFactory>>& factoryList() {
    static std::vector<std::pair<std::string, FeatureFactory>> f;
    return f;
}
} // namespace

void registerFeatureFactory(std::string name, FeatureFactory factory) {
    std::lock_guard lock(factoryMutex());
    for (auto& [n, f] : factoryList()) {
        if (n == name) {
            f = std::move(factory);
            return;
        }
    }
    factoryList().emplace_back(std::move(name), std::move(factory));
}

std::vector<std::pair<std::string, FeatureFactory>> featureFactories() {
    std::lock_guard lock(factoryMutex());
    return factoryList();
}

// --- registry ---

FeatureRegistry::FeatureRegistry() = default;
FeatureRegistry::~FeatureRegistry() = default;

CVar<bool>& FeatureRegistry::toggle(std::string_view featureName) {
    // Process-wide and never destroyed: the cvar registry keeps pointers to them.
    static std::mutex m;
    static std::unordered_map<std::string, std::unique_ptr<CVar<bool>>> toggles;
    std::lock_guard lock(m);
    const std::string name = "r.Feature." + std::string(featureName);
    auto it = toggles.find(name);
    if (it == toggles.end()) {
        it = toggles.emplace(name, std::make_unique<CVar<bool>>(name, true, "Enable the render feature")).first;
    }
    return *it->second;
}

IRenderFeature& FeatureRegistry::add(std::unique_ptr<IRenderFeature> feature) {
    OX_ASSERT(feature, "null feature");
    const std::string_view n = feature->name();
    if (find(n)) OX_LOG_WARN("render", "render feature '{}' registered twice; the newest replaces it", n);
    std::erase_if(m_entries, [&](const Entry& e) { return e.feature->name() == n; });
    (void)toggle(n);
    Entry e;
    e.feature = std::move(feature);
    e.index = m_nextIndex++;
    m_entries.push_back(std::move(e));
    return *m_entries.back().feature;
}

bool FeatureRegistry::remove(std::string_view name, rhi::Device* device) {
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
        if (it->feature->name() == name) {
            if (it->initialized && device) it->feature->shutdown(*device);
            m_entries.erase(it);
            return true;
        }
    }
    return false;
}

IRenderFeature* FeatureRegistry::find(std::string_view name) const {
    for (const Entry& e : m_entries) {
        if (e.feature->name() == name) return e.feature.get();
    }
    return nullptr;
}

std::vector<IRenderFeature*> FeatureRegistry::all() const {
    std::vector<IRenderFeature*> out;
    for (const Entry& e : m_entries) out.push_back(e.feature.get());
    return out;
}

std::vector<IRenderFeature*> FeatureRegistry::resolve(const RenderSettings& settings, const rhi::DeviceCaps& caps) const {
    std::vector<const Entry*> enabled;
    for (const Entry& e : m_entries) {
        if (e.failed) continue;
        if (!toggle(e.feature->name()).get()) continue;
        if (!e.feature->isEnabled(settings, caps)) continue;
        enabled.push_back(&e);
    }
    // Exclusive groups (and the single Upscale slot): keep the highest priority, ties → registered first.
    auto groupOf = [](const IRenderFeature& f) -> std::string {
        if (!f.exclusiveGroup().empty()) return std::string(f.exclusiveGroup());
        if (f.injectionPoints() & maskOf(InjectionPoint::Upscale)) return "__Upscale";
        return {};
    };
    std::unordered_map<std::string, const Entry*> winners;
    for (const Entry* e : enabled) {
        const std::string g = groupOf(*e->feature);
        if (g.empty()) continue;
        auto [it, inserted] = winners.emplace(g, e);
        if (!inserted && e->feature->priority() > it->second->feature->priority()) it->second = e;
    }
    std::vector<IRenderFeature*> out;
    for (const Entry* e : enabled) {
        const std::string g = groupOf(*e->feature);
        if (!g.empty() && winners[g] != e) continue;
        out.push_back(e->feature.get());
    }
    return out;
}

std::vector<IRenderFeature*> FeatureRegistry::at(std::span<IRenderFeature* const> resolved, InjectionPoint point) {
    std::vector<IRenderFeature*> out;
    for (IRenderFeature* f : resolved) {
        if (f->injectionPoints() & maskOf(point)) out.push_back(f);
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const IRenderFeature* a, const IRenderFeature* b) { return a->order() < b->order(); });
    return out;
}

bool FeatureRegistry::ensureInitialized(IRenderFeature& feature, FeatureInitContext& ctx) {
    for (Entry& e : m_entries) {
        if (e.feature.get() != &feature) continue;
        if (e.failed) return false;
        if (!e.initialized) {
            if (!feature.initialize(ctx)) {
                OX_LOG_ERROR("render", "render feature '{}' failed to initialise and is disabled", feature.name());
                e.failed = true;
                return false;
            }
            e.initialized = true;
        }
        return true;
    }
    return false;
}

void FeatureRegistry::shutdownAll(rhi::Device& device) {
    for (Entry& e : m_entries) {
        if (e.initialized) e.feature->shutdown(device);
        e.initialized = false;
    }
}

bool FeatureRegistry::failed(const IRenderFeature& feature) const {
    for (const Entry& e : m_entries) {
        if (e.feature.get() == &feature) return e.failed;
    }
    return false;
}

// --- FeatureContext ---

rhi::RenderGraph& FeatureContext::graph() { return m_frame->view->graph(); }
FrameResources& FeatureContext::resources() { return m_frame->resources; }
RenderView& FeatureContext::view() { return *m_frame->view; }
const RenderSettings& FeatureContext::settings() const { return m_frame->settings; }
const RenderSnapshot& FeatureContext::snapshot() const { return *m_frame->snapshot; }
rhi::Device& FeatureContext::device() { return *m_frame->r->device; }
const rhi::DeviceCaps& FeatureContext::caps() const { return m_frame->r->device->caps(); }
GpuScene& FeatureContext::scene() { return *m_frame->r->scene; }
Renderer& FeatureContext::renderer() { return *m_frame->r->self; }
Extent2D FeatureContext::renderExtent() const { return m_frame->view->renderExtent(); }
Extent2D FeatureContext::outputExtent() const { return m_frame->view->outputExtent(); }
GpuViewConstants& FeatureContext::viewConstants() { return m_frame->constants; }
VkDeviceAddress FeatureContext::viewAddress() const { return m_frame->constantsAlloc.address; }
VkDeviceAddress FeatureContext::sceneAddress() const { return m_frame->headerAlloc.address; }
GpuAllocation FeatureContext::allocate(u64 size, u64 alignment) { return m_frame->r->frameAlloc.allocate(size, alignment); }
const ViewDrawLists& FeatureContext::drawLists() const {
    if (!m_frame->cpuDrawListsBuilt) m_frame->r->buildCpuDrawLists(*m_frame);
    return m_frame->drawLists;
}
const DefaultTextures& FeatureContext::defaults() const { return m_frame->r->cache->defaults(); }
RenderStats& FeatureContext::stats() { return *m_frame->stats; }

void FeatureContext::countDraw(u64 triangles, u32 instances) {
    m_frame->stats->drawCalls += 1;
    m_frame->stats->triangles += triangles * instances;
}

DrawList FeatureContext::buildDrawList(const DrawFilter& filter) {
    DrawList list;
    std::vector<u32> ids;
    m_frame->r->scene->buildDrawList(filter, list, ids);
    list.instanceIds = upload(std::span<const u32>(ids));
    return list;
}

DrawList FeatureContext::cullDrawList(const DrawFilter& filter, u32 instanceMultiplier) {
    GpuDriven* gd = m_frame->r->gpuDriven.get();
    if (gd && m_frame->gpuDriven) return gd->cull(*m_frame, filter, instanceMultiplier);
    return buildDrawList(filter);
}

LodSelection FeatureContext::lodSelection() const {
    const GpuViewConstants& c = m_frame->constants;
    LodSelection s;
    s.cameraPosition = glm::vec3(c.cameraPosition);
    s.orthographic = (c.flags & 1u) != 0;
    s.projScale = 0.5f * c.renderSize.y * std::abs(c.proj[1][1]);
    const f32 pixels = m_frame->r->gpuDriven ? m_frame->r->gpuDriven->settings().lodErrorPixels : 1.0f;
    s.thresholdPixels = pixels * std::exp2(m_frame->settings.lodBias);
    return s;
}

void FeatureContext::drawBatches(rhi::CommandList& cmd, const DrawList& list, std::span<const rhi::PipelineHandle> pipelines,
                                 const void* pushConstants, u32 pushSize, u32 instanceMultiplier) {
    if (list.gpuDriven()) {
        OX_ASSERT(list.indirectMultiplier == std::max(instanceMultiplier, 1u),
                  "indirect list culled for instanceMultiplier {}, drawn with {}", list.indirectMultiplier, instanceMultiplier);
        GpuScene& scene = *m_frame->r->scene;
        const bool multiDraw = m_frame->r->device->caps().multiDrawIndirect;
        cmd.bindIndexBuffer(scene.indexBuffer());
        rhi::PipelineHandle bound;
        bool pushed = false;
        constexpr u32 stride = sizeof(VkDrawIndexedIndirectCommand);
        for (const DrawIndirectRun& r : list.indirectRuns) {
            const rhi::PipelineHandle p = pipelines[std::min<usize>(r.variant, pipelines.size() - 1)];
            if (!p || r.commandCount == 0) continue;
            if (p != bound) {
                cmd.bindPipeline(p);
                bound = p;
                if (!pushed) {
                    cmd.pushConstants(pushConstants, pushSize);
                    pushed = true;
                }
            }
            const u64 offset = list.indirectOffset + u64(r.firstCommand) * stride;
            if (list.indirectCountBuffer && r.countSlot != ~0u) {
                cmd.drawIndexedIndirectCount(list.indirectBuffer, offset, list.indirectCountBuffer,
                                             list.indirectCountOffset + u64(r.countSlot) * 4, r.commandCount, stride);
                m_frame->stats->drawCalls += 1;
                m_frame->stats->indirectDrawCalls += 1;
            } else if (multiDraw) {
                cmd.drawIndexedIndirect(list.indirectBuffer, offset, r.commandCount, stride);
                m_frame->stats->drawCalls += 1;
                m_frame->stats->indirectDrawCalls += 1;
            } else {
                for (u32 c = 0; c < r.commandCount; ++c) cmd.drawIndexedIndirect(list.indirectBuffer, offset + u64(c) * stride, 1, stride);
                m_frame->stats->drawCalls += r.commandCount;
                m_frame->stats->indirectDrawCalls += r.commandCount;
            }
            m_frame->stats->indirectCommands += r.commandCount;
        }
        return;
    }
    if (list.batches.empty()) return;
    GpuScene& scene = *m_frame->r->scene;
    cmd.bindIndexBuffer(scene.indexBuffer());
    rhi::PipelineHandle bound;
    bool pushed = false;
    for (const DrawBatch& b : list.batches) {
        const rhi::PipelineHandle p = pipelines[std::min<usize>(b.variant, pipelines.size() - 1)];
        if (!p) continue;
        if (p != bound) {
            cmd.bindPipeline(p);
            bound = p;
            if (!pushed) {
                cmd.pushConstants(pushConstants, pushSize);
                pushed = true;
            }
        }
        cmd.drawIndexed(b.indexCount, b.instanceCount * instanceMultiplier, b.firstIndex, b.vertexOffset,
                        b.firstInstance * instanceMultiplier);
        countDraw(b.indexCount / 3, b.instanceCount * instanceMultiplier);
    }
}

HistoryTexture FeatureContext::history(std::string_view name, const rhi::TextureDesc& descIn) {
    rhi::Device& dev = device();
    RenderView& v = *m_frame->view;
    HistoryEntry& h = v.impl().histories[std::string(name)];
    rhi::TextureDesc desc = descIn;
    desc.usage = desc.usage | rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    if (desc.name.empty()) desc.name = std::string(name);
    if (!h.textures[0] || !(h.desc == desc)) {
        for (auto& t : h.textures) {
            if (t) dev.destroy(t);
        }
        h.textures[0] = dev.createTexture(desc);
        h.textures[1] = dev.createTexture(desc);
        h.desc = desc;
        h.valid = false;
        h.lastFrame = ~0ull;
    }
    HistoryTexture out;
    if (h.lastFrame != v.frameIndex()) {
        // New frame: the texture written last frame becomes `previous`.
        if (h.lastFrame != ~0ull) h.current ^= 1u;
        out.previousValid = h.lastFrame != ~0ull && h.lastFrame + 1 == v.frameIndex() && !v.cameraCut();
        h.valid = out.previousValid;
        h.lastFrame = v.frameIndex();
    } else {
        out.previousValid = h.valid;
    }
    out.current = importPersistent(graph(), dev, h.textures[h.current]);
    out.previous = importPersistent(graph(), dev, h.textures[h.current ^ 1u]);
    return out;
}

IFeatureViewState*& FeatureContext::viewStateSlot(std::type_index type) {
    return m_frame->view->impl().featureStates[{m_feature, type}];
}

// --- helpers ---

rhi::ShaderStageDesc fullscreenVertexShader() { return rhi::ShaderStageDesc::file("render/common/fullscreen.vert"); }

rhi::PipelineHandle createFullscreenPipeline(rhi::Device& device, std::string name, std::string fragmentPath,
                                             std::vector<VkFormat> colorFormats, std::vector<rhi::BlendState> blend,
                                             std::vector<rhi::ShaderDefine> defines) {
    rhi::GraphicsPipelineDesc d;
    d.name = std::move(name);
    d.vertex = fullscreenVertexShader();
    d.fragment = rhi::ShaderStageDesc::file(std::move(fragmentPath), std::move(defines));
    d.colorFormats = std::move(colorFormats);
    d.blend = std::move(blend);
    return device.createGraphicsPipeline(d);
}

rhi::PipelineHandle createComputePipeline(rhi::Device& device, std::string name, std::string path,
                                          std::vector<rhi::ShaderDefine> defines) {
    rhi::ComputePipelineDesc d;
    d.name = std::move(name);
    d.shader = rhi::ShaderStageDesc::file(std::move(path), std::move(defines));
    return device.createComputePipeline(d);
}

void drawFullscreen(rhi::CommandList& cmd, rhi::PipelineHandle pipeline, const void* pushConstants, u32 pushSize) {
    if (pipeline) cmd.bindPipeline(pipeline);
    if (pushConstants && pushSize) cmd.pushConstants(pushConstants, pushSize);
    cmd.draw(3);
}

rhi::RGTexture importPersistent(rhi::RenderGraph& graph, rhi::Device& device, rhi::TextureHandle texture) {
    return graph.importTexture(device, texture);
}

Renderer::Impl& rendererImpl(FeatureContext& ctx) { return ctx.renderer().impl(); }
FrameState& frameState(FeatureContext& ctx) { return ctx.internalFrame(); }

} // namespace ox::render
