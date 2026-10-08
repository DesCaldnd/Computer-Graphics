// Render graph declaration + compilation. Pure CPU: no Vulkan calls here, so the planner is unit-testable
// without a GPU. Execution lives in render_graph_execute.cpp.
#include "render_graph_impl.hpp"

#include <oxwald/core/assert.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/rhi/format.hpp>
#include <oxwald/rhi/shader_compiler.hpp>

#include <algorithm>
#include <format>
#include <sstream>

namespace ox::rhi {

const char* passTypeName(PassType t) {
    switch (t) {
    case PassType::Graphics: return "graphics";
    case PassType::Compute: return "compute";
    case PassType::Transfer: return "transfer";
    case PassType::RayTracing: return "raytracing";
    }
    return "?";
}

// ---------------------------------------------------------------------------------------------------------------
// PassBuilder

namespace {

void addAccess(RGPassDecl& pass, u32 resource, bool texture, Access access, bool preserve, const std::string& resName) {
    for (auto& a : pass.accesses) {
        if (a.resource == resource && a.texture == texture) {
            OX_ASSERT(a.access == access, "pass '{}' declares '{}' twice with different accesses ({} vs {})", pass.name,
                      resName, accessName(a.access), accessName(access));
            a.preserve = a.preserve || preserve;
            return;
        }
    }
    pass.accesses.push_back({resource, texture, access, preserve});
}

} // namespace

PassBuilder& PassBuilder::read(RGTexture t, Access access) {
    OX_ASSERT(t.valid() && !isWrite(access), "read() needs a valid texture and a read access");
    auto& g = *m_graph->m_impl;
    addAccess(g.passes[m_pass], t.id, true, access, true, g.resources[t.id].name);
    return *this;
}
PassBuilder& PassBuilder::read(RGBuffer b, Access access) {
    OX_ASSERT(b.valid() && !isWrite(access), "read() needs a valid buffer and a read access");
    auto& g = *m_graph->m_impl;
    addAccess(g.passes[m_pass], b.id, false, access, true, g.resources[b.id].name);
    return *this;
}
PassBuilder& PassBuilder::write(RGTexture t, Access access) {
    OX_ASSERT(t.valid());
    auto& g = *m_graph->m_impl;
    addAccess(g.passes[m_pass], t.id, true, access, true, g.resources[t.id].name);
    return *this;
}
PassBuilder& PassBuilder::write(RGBuffer b, Access access) {
    OX_ASSERT(b.valid());
    auto& g = *m_graph->m_impl;
    addAccess(g.passes[m_pass], b.id, false, access, true, g.resources[b.id].name);
    return *this;
}
PassBuilder& PassBuilder::overwrite(RGTexture t, Access access) {
    OX_ASSERT(t.valid() && isWrite(access), "overwrite() needs a write access");
    auto& g = *m_graph->m_impl;
    addAccess(g.passes[m_pass], t.id, true, access, false, g.resources[t.id].name);
    return *this;
}
PassBuilder& PassBuilder::overwrite(RGBuffer b, Access access) {
    OX_ASSERT(b.valid() && isWrite(access), "overwrite() needs a write access");
    auto& g = *m_graph->m_impl;
    addAccess(g.passes[m_pass], b.id, false, access, false, g.resources[b.id].name);
    return *this;
}
PassBuilder& PassBuilder::color(RGTexture t, VkAttachmentLoadOp load, ClearColor clear, u32 mip, u32 layer) {
    OX_ASSERT(t.valid());
    auto& g = *m_graph->m_impl;
    auto& pass = g.passes[m_pass];
    addAccess(pass, t.id, true, Access::ColorAttachmentWrite, load == VK_ATTACHMENT_LOAD_OP_LOAD, g.resources[t.id].name);
    RGAttachmentDecl a;
    a.resource = t.id;
    a.load = load;
    a.clear = clear;
    a.mip = mip;
    a.layer = layer;
    pass.colors.push_back(a);
    return *this;
}
PassBuilder& PassBuilder::depth(RGTexture t, VkAttachmentLoadOp load, ClearDepthStencil clear, bool readOnly) {
    OX_ASSERT(t.valid());
    auto& g = *m_graph->m_impl;
    auto& pass = g.passes[m_pass];
    if (readOnly) {
        addAccess(pass, t.id, true, Access::DepthStencilRead, true, g.resources[t.id].name);
        load = VK_ATTACHMENT_LOAD_OP_LOAD;
    } else {
        addAccess(pass, t.id, true, Access::DepthStencilWrite, load == VK_ATTACHMENT_LOAD_OP_LOAD, g.resources[t.id].name);
    }
    RGAttachmentDecl a;
    a.resource = t.id;
    a.load = load;
    a.clearDepth = clear;
    a.readOnly = readOnly;
    pass.depth = a;
    return *this;
}
PassBuilder& PassBuilder::resolve(RGTexture msaaColor, RGTexture target) {
    auto& g = *m_graph->m_impl;
    auto& pass = g.passes[m_pass];
    for (auto& c : pass.colors) {
        if (c.resource == msaaColor.id) {
            c.resolveTarget = target.id;
            addAccess(pass, target.id, true, Access::ColorAttachmentWrite, false, g.resources[target.id].name);
            return *this;
        }
    }
    OX_ASSERT(false, "resolve(): '{}' is not a color attachment of pass '{}'", g.resources[msaaColor.id].name, pass.name);
    return *this;
}
PassBuilder& PassBuilder::queue(QueueType hint) {
    m_graph->m_impl->passes[m_pass].queueHint = hint;
    return *this;
}
PassBuilder& PassBuilder::sideEffect() {
    m_graph->m_impl->passes[m_pass].sideEffect = true;
    return *this;
}
PassBuilder& PassBuilder::secondaryCommandLists() {
    m_graph->m_impl->passes[m_pass].secondary = true;
    return *this;
}
PassBuilder& PassBuilder::execute(std::function<void(PassContext&)> fn) {
    m_graph->m_impl->passes[m_pass].fn = std::move(fn);
    return *this;
}

// ---------------------------------------------------------------------------------------------------------------
// RenderGraph declarations

RenderGraph::RenderGraph() : m_impl(std::make_unique<Impl>()) {}
RenderGraph::~RenderGraph() {
    if (m_impl->physicalDevice) {
        releaseResources(*m_impl->physicalDevice);
    }
}

void RenderGraph::reset() {
    m_impl->passes.clear();
    m_impl->resources.clear();
}

RGTexture RenderGraph::createTexture(const TextureDesc& desc) {
    RGResourceDecl r;
    r.name = desc.name.empty() ? std::format("texture{}", m_impl->resources.size()) : desc.name;
    r.texture = true;
    r.textureDesc = desc;
    if (r.textureDesc.mipLevels == 0) {
        r.textureDesc.mipLevels = fullMipCount(desc.width, desc.height, desc.depth);
    }
    m_impl->resources.push_back(std::move(r));
    return {u32(m_impl->resources.size() - 1)};
}

RGBuffer RenderGraph::createBuffer(const BufferDesc& desc) {
    RGResourceDecl r;
    r.name = desc.name.empty() ? std::format("buffer{}", m_impl->resources.size()) : desc.name;
    r.texture = false;
    r.bufferDesc = desc;
    m_impl->resources.push_back(std::move(r));
    return {u32(m_impl->resources.size() - 1)};
}

RGTexture RenderGraph::importTexture(TextureHandle texture, const TextureDesc& desc, RGImport import) {
    RGTexture t = createTexture(desc);
    auto& r = m_impl->resources[t.id];
    r.imported = true;
    r.importedTexture = texture;
    r.import = import;
    return t;
}

RGBuffer RenderGraph::importBuffer(BufferHandle buffer, const BufferDesc& desc, RGImport import) {
    RGBuffer b = createBuffer(desc);
    auto& r = m_impl->resources[b.id];
    r.imported = true;
    r.importedBuffer = buffer;
    r.import = import;
    return b;
}

void RenderGraph::markOutput(RGTexture t) { m_impl->resources.at(t.id).output = true; }
void RenderGraph::markOutput(RGBuffer b) { m_impl->resources.at(b.id).output = true; }

PassBuilder RenderGraph::addPass(std::string name, PassType type) {
    RGPassDecl p;
    p.name = std::move(name);
    p.type = type;
    if (type == PassType::Compute) {
        p.queueHint = QueueType::Graphics; // async only on explicit request
    }
    m_impl->passes.push_back(std::move(p));
    return PassBuilder(*this, u32(m_impl->passes.size() - 1));
}

u32 RenderGraph::passCount() const { return u32(m_impl->passes.size()); }
const std::string& RenderGraph::passName(u32 pass) const { return m_impl->passes.at(pass).name; }
u32 RenderGraph::resourceCount() const { return u32(m_impl->resources.size()); }
const std::string& RenderGraph::resourceName(u32 r) const { return m_impl->resources.at(r).name; }
const RenderGraphPlan& RenderGraph::plan() const { return m_impl->plan; }
u32 RenderGraph::compileCount() const { return m_impl->compileCount; }

// ---------------------------------------------------------------------------------------------------------------
// Compilation

namespace {

template <class T>
u64 mix(u64 h, const T& v) {
    static_assert(std::is_trivially_copyable_v<T>);
    return hashBytes(&v, sizeof(v), h);
}
u64 mixStr(u64 h, std::string_view s) { return hashBytes(s.data(), s.size(), mix(h, s.size())); }

constexpr VkAccessFlags2 kWriteAccessBits =
    VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT |
    VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;

TextureUsage textureUsageFor(Access a) {
    switch (a) {
    case Access::ColorAttachmentWrite: return TextureUsage::ColorAttachment;
    case Access::DepthStencilWrite: return TextureUsage::DepthStencilAttachment;
    case Access::DepthStencilRead: return TextureUsage::DepthStencilAttachment | TextureUsage::Sampled;
    case Access::SampledGraphics: case Access::SampledFragment: case Access::SampledCompute: case Access::SampledRayTracing:
        return TextureUsage::Sampled;
    case Access::StorageReadGraphics: case Access::StorageReadCompute: case Access::StorageReadRayTracing:
    case Access::StorageWriteGraphics: case Access::StorageWriteCompute: case Access::StorageWriteRayTracing:
        return TextureUsage::Storage;
    case Access::TransferRead: return TextureUsage::TransferSrc;
    case Access::TransferWrite: return TextureUsage::TransferDst;
    case Access::General: return TextureUsage::Sampled | TextureUsage::Storage | TextureUsage::TransferSrc | TextureUsage::TransferDst;
    default: return TextureUsage::None;
    }
}

BufferUsage bufferUsageFor(Access a) {
    switch (a) {
    case Access::VertexBuffer: return BufferUsage::Vertex;
    case Access::IndexBuffer: return BufferUsage::Index;
    case Access::IndirectBuffer: return BufferUsage::Indirect;
    case Access::UniformGraphics: case Access::UniformCompute: return BufferUsage::Uniform;
    case Access::TransferRead: return BufferUsage::TransferSrc;
    case Access::TransferWrite: return BufferUsage::TransferDst;
    case Access::AccelStructBuildRead: return BufferUsage::AccelStructInput;
    case Access::General: return BufferUsage::Storage | BufferUsage::TransferSrc | BufferUsage::TransferDst;
    default: return BufferUsage::Storage;
    }
}

struct ResState {
    bool init = false;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    Access lastAccess = Access::Undefined;
    VkPipelineStageFlags2 writeStages = 0;
    VkAccessFlags2 writeAccess = 0;
    VkPipelineStageFlags2 readStages = 0;
    VkPipelineStageFlags2 syncedStages = 0;
    VkAccessFlags2 syncedAccess = 0;
    QueueType queue = QueueType::Graphics;
    i32 lastPass = -1;
    i32 lastBatch = -1;
    i32 writerBatch = -1;
    std::vector<i32> readerBatches;
    // First barrier of a transient resource (patched with the slot's end-of-frame state for cross-frame WAR).
    i32 firstBarrierPass = -1;
    i32 firstBarrierIndex = -1;
};

void addWait(RGBatch& batch, u32 self, i32 other) {
    if (other < 0 || u32(other) == self) {
        return;
    }
    if (std::find(batch.waitBatches.begin(), batch.waitBatches.end(), u32(other)) == batch.waitBatches.end()) {
        batch.waitBatches.push_back(u32(other));
    }
}

} // namespace

u64 RenderGraph::Impl::computeHash(const RGCompileOptions& o) const {
    u64 h = 0xcbf29ce484222325ull;
    h = mix(h, o.cull);
    h = mix(h, o.aliasing);
    h = mix(h, o.asyncCompute);
    h = mix(h, o.asyncTransfer);
    h = mix(h, o.queueFamilies);
    for (const auto& r : resources) {
        h = mixStr(h, r.name);
        h = mix(h, r.texture);
        h = mix(h, r.imported);
        h = mix(h, r.output);
        h = mix(h, r.import.initial);
        h = mix(h, r.import.final);
        if (r.texture) {
            const auto& d = r.textureDesc;
            h = mix(h, d.type);
            h = mix(h, d.format);
            h = mix(h, d.width);
            h = mix(h, d.height);
            h = mix(h, d.depth);
            h = mix(h, d.mipLevels);
            h = mix(h, d.arrayLayers);
            h = mix(h, d.samples);
            h = mix(h, d.usage);
        } else {
            h = mix(h, r.bufferDesc.size);
            h = mix(h, r.bufferDesc.usage);
            h = mix(h, r.bufferDesc.memory);
        }
    }
    for (const auto& p : passes) {
        h = mixStr(h, p.name);
        h = mix(h, p.type);
        h = mix(h, p.queueHint);
        h = mix(h, p.sideEffect);
        for (const auto& a : p.accesses) {
            h = mix(h, a.resource);
            h = mix(h, a.texture);
            h = mix(h, a.access);
            h = mix(h, a.preserve);
        }
    }
    return h;
}

const RenderGraphPlan& RenderGraph::compile(const RGCompileOptions& options) {
    const u64 h = m_impl->computeHash(options);
    if (m_impl->planValid && m_impl->plan.hash == h) {
        return m_impl->plan;
    }
    m_impl->compileFull(options);
    m_impl->plan.hash = h;
    m_impl->planValid = true;
    ++m_impl->compileCount;
    return m_impl->plan;
}

void RenderGraph::Impl::compileFull(const RGCompileOptions& o) {
    RenderGraphPlan out;
    const u32 passCount = u32(passes.size());
    const u32 resCount = u32(resources.size());

    auto effectiveQueue = [&](const RGPassDecl& p) {
        if (p.queueHint == QueueType::Compute && o.asyncCompute) return QueueType::Compute;
        if (p.queueHint == QueueType::Transfer && o.asyncTransfer) return QueueType::Transfer;
        return QueueType::Graphics;
    };

    // 1. Dependencies (reader -> most recent writer) and culling roots.
    std::vector<std::vector<u32>> deps(passCount);
    std::vector<i32> lastWriter(resCount, -1);
    std::vector<bool> alive(passCount, !o.cull);
    std::vector<u32> roots;
    for (u32 p = 0; p < passCount; ++p) {
        bool root = passes[p].sideEffect;
        for (const auto& a : passes[p].accesses) {
            const bool w = isWrite(a.access);
            if ((!w || a.preserve) && lastWriter[a.resource] >= 0) {
                deps[p].push_back(u32(lastWriter[a.resource]));
            }
            if (w) {
                lastWriter[a.resource] = i32(p);
                if (resources[a.resource].imported || resources[a.resource].output) {
                    root = true;
                }
            }
        }
        if (root) {
            roots.push_back(p);
        }
    }
    if (o.cull) {
        std::vector<u32> stack = roots;
        while (!stack.empty()) {
            u32 p = stack.back();
            stack.pop_back();
            if (alive[p]) continue;
            alive[p] = true;
            for (u32 d : deps[p]) {
                if (!alive[d]) stack.push_back(d);
            }
        }
    }

    // 2. Order (declaration order is a valid topological order) and batches. A batch is also split before the first
    // pass that depends on another queue's batch the current batch does not wait for yet: semaphore waits apply to
    // whole submissions, so the independent passes before it keep overlapping with async compute.
    {
        std::vector<i32> writerBatch(resCount, -1), lastBatch(resCount, -1);
        std::vector<std::vector<i32>> readerBatches(resCount);
        std::vector<i32> batchDeps, need;
        for (u32 p = 0; p < passCount; ++p) {
            if (!alive[p]) {
                out.culledPasses.push_back(p);
                continue;
            }
            RGPlannedPass pp;
            pp.pass = p;
            pp.queue = effectiveQueue(passes[p]);
            need.clear();
            for (const auto& a : passes[p].accesses) {
                const i32 wb = writerBatch[a.resource];
                if (wb >= 0 && out.batches[u32(wb)].queue != pp.queue) need.push_back(wb);
                // Any access after another queue's (even read after read) waits for that batch: the resource
                // changes its owning queue (step 5).
                const i32 lb = lastBatch[a.resource];
                if (lb >= 0 && out.batches[u32(lb)].queue != pp.queue) need.push_back(lb);
                if (isWrite(a.access)) {
                    for (i32 rb : readerBatches[a.resource]) {
                        if (out.batches[u32(rb)].queue != pp.queue) need.push_back(rb);
                    }
                }
            }
            bool split = out.batches.empty() || out.batches.back().queue != pp.queue;
            if (!split && !out.batches.back().passes.empty()) {
                for (i32 n : need) {
                    if (std::find(batchDeps.begin(), batchDeps.end(), n) == batchDeps.end()) split = true;
                }
            }
            if (split) {
                out.batches.push_back({pp.queue, {}, {}});
                batchDeps.clear();
            }
            batchDeps.insert(batchDeps.end(), need.begin(), need.end());
            pp.batch = u32(out.batches.size() - 1);
            for (const auto& a : passes[p].accesses) {
                if (isWrite(a.access)) {
                    writerBatch[a.resource] = i32(pp.batch);
                    readerBatches[a.resource].clear();
                } else {
                    readerBatches[a.resource].push_back(i32(pp.batch));
                }
                lastBatch[a.resource] = i32(pp.batch);
            }
            out.batches.back().passes.push_back(u32(out.passes.size()));
            out.passes.push_back(std::move(pp));
        }
    }

    // 3. Lifetimes, derived usage, memory requirements.
    out.resources.resize(resCount);
    for (u32 r = 0; r < resCount; ++r) {
        auto& rp = out.resources[r];
        rp.texture = resources[r].texture;
        rp.imported = resources[r].imported;
    }
    for (u32 i = 0; i < out.passes.size(); ++i) {
        const auto& decl = passes[out.passes[i].pass];
        for (const auto& a : decl.accesses) {
            auto& rp = out.resources[a.resource];
            if (!rp.used) {
                rp.firstPass = i;
            }
            rp.used = true;
            rp.lastPass = i;
            if (out.passes[i].queue != QueueType::Graphics) {
                rp.asyncQueue = true;
            }
            if (rp.texture) {
                rp.derivedTextureUsage |= textureUsageFor(a.access);
            } else {
                rp.derivedBufferUsage |= bufferUsageFor(a.access);
            }
        }
    }
    for (u32 r = 0; r < resCount; ++r) {
        auto& rp = out.resources[r];
        const auto& decl = resources[r];
        if (decl.imported || !rp.used) continue;
        MemoryRequirements req;
        if (rp.texture) {
            TextureDesc d = decl.textureDesc;
            d.usage |= rp.derivedTextureUsage;
            req = o.textureRequirements ? o.textureRequirements(d) : MemoryRequirements{estimateTextureSize(d), 65536, ~0u};
        } else {
            BufferDesc d = decl.bufferDesc;
            d.usage |= rp.derivedBufferUsage;
            req = o.bufferRequirements ? o.bufferRequirements(d) : MemoryRequirements{(d.size + 255) & ~u64(255), 256, ~0u};
        }
        rp.size = req.size;
        rp.alignment = req.alignment;
        rp.memoryTypeBits = req.memoryTypeBits;
        out.transientBytesUnaliased += req.size;
    }

    // 4. Aliasing: greedy interval packing of transient resources into memory slots.
    {
        std::vector<u32> candidates;
        for (u32 r = 0; r < resCount; ++r) {
            const auto& rp = out.resources[r];
            if (!rp.imported && rp.used) candidates.push_back(r);
        }
        std::sort(candidates.begin(), candidates.end(), [&](u32 a, u32 b) {
            const auto& ra = out.resources[a];
            const auto& rb = out.resources[b];
            return ra.size != rb.size ? ra.size > rb.size : ra.firstPass < rb.firstPass;
        });
        auto overlaps = [&](u32 a, u32 b) {
            const auto& ra = out.resources[a];
            const auto& rb = out.resources[b];
            return !(ra.lastPass < rb.firstPass || rb.lastPass < ra.firstPass);
        };
        for (u32 r : candidates) {
            auto& rp = out.resources[r];
            const bool aliasable = o.aliasing && !rp.asyncQueue && !resources[r].output &&
                                   (rp.texture || resources[r].bufferDesc.memory == MemoryUsage::GpuOnly);
            i32 best = -1;
            u64 bestWaste = ~0ull;
            if (aliasable) {
                for (u32 s = 0; s < out.aliasSlots.size(); ++s) {
                    const auto& slot = out.aliasSlots[s];
                    if (slot.texture != rp.texture || (slot.memoryTypeBits & rp.memoryTypeBits) == 0) continue;
                    if (slot.resources.empty()) continue;
                    const auto& first = out.resources[slot.resources.front()];
                    if (first.asyncQueue || resources[slot.resources.front()].output) continue;
                    bool free = true;
                    for (u32 other : slot.resources) {
                        if (overlaps(r, other)) {
                            free = false;
                            break;
                        }
                    }
                    if (!free) continue;
                    const u64 waste = slot.size > rp.size ? slot.size - rp.size : rp.size - slot.size;
                    if (waste < bestWaste) {
                        bestWaste = waste;
                        best = i32(s);
                    }
                }
            }
            if (best < 0) {
                RGAliasSlot slot;
                slot.texture = rp.texture;
                slot.memoryTypeBits = rp.memoryTypeBits;
                out.aliasSlots.push_back(slot);
                best = i32(out.aliasSlots.size() - 1);
            }
            auto& slot = out.aliasSlots[best];
            slot.size = std::max(slot.size, rp.size);
            slot.alignment = std::max(slot.alignment, rp.alignment);
            slot.memoryTypeBits &= rp.memoryTypeBits;
            slot.resources.push_back(r);
            rp.aliasSlot = best;
        }
        for (auto& slot : out.aliasSlots) {
            std::sort(slot.resources.begin(), slot.resources.end(),
                      [&](u32 a, u32 b) { return out.resources[a].firstPass < out.resources[b].firstPass; });
            out.transientBytesAliased += slot.size;
        }
    }
    // Predecessor within the alias slot (previous lifetime).
    std::vector<i32> aliasPredecessor(resCount, -1);
    for (const auto& slot : out.aliasSlots) {
        for (usize i = 1; i < slot.resources.size(); ++i) {
            aliasPredecessor[slot.resources[i]] = i32(slot.resources[i - 1]);
        }
    }

    // 5. Barriers: walk passes in execution order and track per-resource state.
    std::vector<ResState> states(resCount);
    auto familyOf = [&](QueueType q) { return o.queueFamilies[u32(q)]; };

    for (u32 i = 0; i < out.passes.size(); ++i) {
        RGPlannedPass& pp = out.passes[i];
        const RGPassDecl& decl = passes[pp.pass];
        const QueueType q = pp.queue;
        RGBatch& batch = out.batches[pp.batch];

        for (const auto& a : decl.accesses) {
            ResState& st = states[a.resource];
            const RGResourceDecl& res = resources[a.resource];
            const AccessInfo info = accessInfo(a.access);
            RGBarrier br;
            br.resource = a.resource;
            br.texture = res.texture;
            br.dstAccess = a.access;
            br.dstStages = info.stages;
            br.dstAccessMask = info.access;
            br.newLayout = res.texture ? info.layout : VK_IMAGE_LAYOUT_UNDEFINED;

            auto updateState = [&](bool layoutChanged) {
                if (info.write) {
                    st.writeStages = info.stages;
                    st.writeAccess = info.access & kWriteAccessBits;
                    st.readStages = 0;
                    st.syncedStages = 0;
                    st.syncedAccess = 0;
                    st.readerBatches.clear();
                    st.writerBatch = i32(pp.batch);
                } else {
                    if (layoutChanged) {
                        // The layout transition acts as a write ordered before these stages; the previous writes
                        // were made available by that barrier, so later readers only need an execution dependency.
                        st.writeStages = info.stages;
                        st.writeAccess = 0;
                        st.readStages = info.stages;
                        st.syncedStages = info.stages;
                        st.syncedAccess = info.access;
                    } else {
                        st.readStages |= info.stages;
                        st.syncedStages |= info.stages;
                        st.syncedAccess |= info.access;
                    }
                    st.readerBatches.push_back(i32(pp.batch));
                }
                if (res.texture) st.layout = info.layout;
                st.lastAccess = a.access;
                st.queue = q;
                st.lastPass = i32(i);
                st.lastBatch = i32(pp.batch);
            };

            if (!st.init) {
                st.init = true;
                if (res.imported) {
                    const AccessInfo ii = accessInfo(res.import.initial);
                    st.layout = res.texture ? ii.layout : VK_IMAGE_LAYOUT_UNDEFINED;
                    st.queue = QueueType::Graphics;
                    st.lastAccess = res.import.initial;
                    if (ii.write) {
                        st.writeStages = ii.stages;
                        st.writeAccess = ii.access & kWriteAccessBits;
                    } else {
                        st.readStages = ii.stages;
                        st.syncedStages = ii.stages;
                        st.syncedAccess = ii.access;
                    }
                    if (res.import.initial == Access::Undefined) {
                        // Discard: only a layout transition, chained after whatever waited before the graph.
                        br.srcAccess = Access::Undefined;
                        br.srcStages = info.stages;
                        br.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                        if (res.texture) pp.before.push_back(br);
                        updateState(true);
                        continue;
                    }
                    if (q != QueueType::Graphics && familyOf(q) != familyOf(QueueType::Graphics)) {
                        out.warnings.push_back(std::format("imported '{}' first used on the {} queue; ownership is "
                                                           "assumed to be on the graphics family",
                                                           res.name, queueTypeName(q)));
                    }
                    // Fall through to the regular rules using the imported state.
                } else {
                    const i32 pred = aliasPredecessor[a.resource];
                    br.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                    if (pred >= 0) {
                        const ResState& ps = states[pred];
                        br.kind = RGBarrierKind::Aliasing;
                        br.srcAccess = ps.lastAccess;
                        br.srcStages = ps.writeStages | ps.readStages;
                        br.srcAccessMask = ps.writeAccess;
                    } else {
                        br.srcAccess = Access::Undefined;
                    }
                    if (!info.write || a.preserve) {
                        out.warnings.push_back(std::format("pass '{}' reads transient '{}' before anything wrote it",
                                                           decl.name, res.name));
                    }
                    st.firstBarrierPass = i32(i);
                    st.firstBarrierIndex = i32(pp.before.size());
                    pp.before.push_back(br);
                    updateState(true);
                    continue;
                }
            }

            // Cross-queue dependencies (semaphores between batches).
            const bool crossQueue = st.queue != q;
            if (info.write) {
                for (i32 rb : st.readerBatches) {
                    if (out.batches[u32(rb)].queue != q) addWait(batch, pp.batch, rb);
                }
            }
            if (st.writerBatch >= 0 && out.batches[u32(st.writerBatch)].queue != q) {
                addWait(batch, pp.batch, st.writerBatch);
            }
            if (crossQueue && st.lastBatch >= 0) {
                addWait(batch, pp.batch, st.lastBatch);
            }

            const bool layoutChange = res.texture && st.layout != info.layout;
            br.srcAccess = st.lastAccess;
            br.oldLayout = res.texture ? st.layout : VK_IMAGE_LAYOUT_UNDEFINED;

            if (crossQueue && st.lastPass >= 0) {
                const u32 srcFam = familyOf(st.queue);
                const u32 dstFam = familyOf(q);
                if (srcFam != dstFam) {
                    RGBarrier rel = br;
                    rel.kind = RGBarrierKind::Release;
                    rel.srcStages = st.writeStages | st.readStages;
                    rel.srcAccessMask = st.writeAccess;
                    rel.dstStages = 0;
                    rel.dstAccessMask = 0;
                    rel.srcFamily = srcFam;
                    rel.dstFamily = dstFam;
                    out.passes[u32(st.lastPass)].after.push_back(rel);
                    RGBarrier acq = br;
                    acq.kind = RGBarrierKind::Acquire;
                    acq.srcStages = 0;
                    acq.srcAccessMask = 0;
                    acq.srcFamily = srcFam;
                    acq.dstFamily = dstFam;
                    pp.before.push_back(acq);
                } else if (layoutChange) {
                    // The semaphore wait covers memory; only the layout transition is needed on this queue.
                    br.srcStages = info.stages;
                    br.srcAccessMask = 0;
                    pp.before.push_back(br);
                }
                if (info.write) {
                    updateState(layoutChange);
                } else {
                    st.writeStages = info.stages;
                    st.readStages = 0;
                    st.syncedStages = 0;
                    st.syncedAccess = 0;
                    updateState(true);
                }
                continue;
            }

            if (info.write || layoutChange) {
                br.srcStages = st.writeStages | st.readStages;
                br.srcAccessMask = st.writeAccess;
                if (br.srcStages != 0 || layoutChange) {
                    pp.before.push_back(br);
                }
            } else if (st.writeStages != 0 &&
                       ((info.stages & ~st.syncedStages) != 0 || (info.access & ~st.syncedAccess) != 0)) {
                br.srcStages = st.writeStages;
                br.srcAccessMask = st.writeAccess;
                pp.before.push_back(br);
            }
            updateState(layoutChange);
        }
    }

    // 6. Final transitions for imported resources; record end states.
    for (u32 r = 0; r < resCount; ++r) {
        const RGResourceDecl& res = resources[r];
        ResState& st = states[r];
        auto& rp = out.resources[r];
        rp.finalAccess = st.init ? st.lastAccess : (res.imported ? res.import.initial : Access::Undefined);
        if (!res.imported) continue;
        if (!st.init) {
            if (res.import.final != Access::Undefined && res.import.final != res.import.initial) {
                out.warnings.push_back(std::format("imported '{}' is not used by any surviving pass", res.name));
            }
            continue;
        }
        if (res.import.final == Access::Undefined || res.import.final == st.lastAccess) continue;
        if (st.queue != QueueType::Graphics) {
            out.warnings.push_back(std::format("final transition of '{}' recorded on the {} queue", res.name,
                                               queueTypeName(st.queue)));
        }
        const AccessInfo fi = accessInfo(res.import.final);
        RGBarrier br;
        br.resource = r;
        br.texture = res.texture;
        br.kind = RGBarrierKind::Final;
        br.srcAccess = st.lastAccess;
        br.dstAccess = res.import.final;
        br.srcStages = st.writeStages | st.readStages;
        br.srcAccessMask = st.writeAccess;
        br.dstStages = fi.stages;
        br.dstAccessMask = fi.access;
        br.oldLayout = res.texture ? st.layout : VK_IMAGE_LAYOUT_UNDEFINED;
        br.newLayout = res.texture ? fi.layout : VK_IMAGE_LAYOUT_UNDEFINED;
        out.passes[u32(st.lastPass)].after.push_back(br);
        rp.finalAccess = res.import.final;
    }

    // 7. Cross-frame WAR: the first barrier of each alias slot waits for the slot's last user of the previous frame.
    for (const auto& slot : out.aliasSlots) {
        const ResState& first = states[slot.resources.front()];
        const ResState& last = states[slot.resources.back()];
        if (first.firstBarrierPass < 0) continue;
        // Stages of another queue family are not valid in this queue's barrier (e.g. fragment stages on a dedicated
        // compute family); there the execution waits on the previous frame's submissions of the other queues instead.
        if (familyOf(out.passes[u32(first.firstBarrierPass)].queue) != familyOf(last.queue)) continue;
        RGBarrier& br = out.passes[u32(first.firstBarrierPass)].before[u32(first.firstBarrierIndex)];
        br.srcStages |= last.writeStages | last.readStages;
        br.srcAccessMask |= last.writeAccess;
    }

    for (auto& b : out.batches) {
        std::sort(b.waitBatches.begin(), b.waitBatches.end());
    }
    plan = std::move(out);
}

// ---------------------------------------------------------------------------------------------------------------
// Plan helpers, dumps

u32 RenderGraphPlan::barrierCount() const {
    u32 n = 0;
    for (const auto& p : passes) n += u32(p.before.size() + p.after.size());
    return n;
}

const RGPlannedPass* RenderGraphPlan::findPass(u32 declaredPass) const {
    for (const auto& p : passes) {
        if (p.pass == declaredPass) return &p;
    }
    return nullptr;
}

namespace {
const char* barrierKindName(RGBarrierKind k) {
    switch (k) {
    case RGBarrierKind::Normal: return "barrier";
    case RGBarrierKind::Aliasing: return "alias";
    case RGBarrierKind::Release: return "release";
    case RGBarrierKind::Acquire: return "acquire";
    case RGBarrierKind::Final: return "final";
    }
    return "?";
}
} // namespace

std::string RenderGraphPlan::dump(const RenderGraph& g) const {
    std::ostringstream s;
    for (const auto& p : passes) {
        s << std::format("[{}] {} (batch {}, {})\n", p.pass, g.passName(p.pass), p.batch, queueTypeName(p.queue));
        auto line = [&](const RGBarrier& br, const char* where) {
            s << std::format("    {} {} '{}': {} -> {} (layout {} -> {})\n", where, barrierKindName(br.kind),
                             g.resourceName(br.resource), accessName(br.srcAccess), accessName(br.dstAccess),
                             int(br.oldLayout), int(br.newLayout));
        };
        for (const auto& br : p.before) line(br, "before");
        for (const auto& br : p.after) line(br, "after ");
    }
    for (u32 c : culledPasses) {
        s << std::format("culled: {}\n", g.passName(c));
    }
    for (u32 i = 0; i < aliasSlots.size(); ++i) {
        s << std::format("slot {} ({} KiB):", i, aliasSlots[i].size / 1024);
        for (u32 r : aliasSlots[i].resources) s << ' ' << g.resourceName(r);
        s << '\n';
    }
    for (const auto& w : warnings) s << "warning: " << w << '\n';
    return s.str();
}

std::string RenderGraph::exportGraphviz() const {
    const auto& im = *m_impl;
    std::ostringstream s;
    s << "digraph RenderGraph {\n  rankdir=LR;\n  node [fontname=\"Helvetica\"];\n";
    std::vector<bool> culled(im.passes.size(), false);
    for (u32 c : im.plan.culledPasses) {
        if (c < culled.size()) culled[c] = true;
    }
    for (u32 p = 0; p < im.passes.size(); ++p) {
        const auto& pass = im.passes[p];
        const char* color = pass.queueHint == QueueType::Compute ? "lightblue"
                            : pass.queueHint == QueueType::Transfer ? "khaki" : "palegreen";
        s << std::format("  p{} [shape=box, style=\"filled{}\", fillcolor={}, label=\"{}\\n({})\"];\n", p,
                         culled[p] ? ",dashed" : "", culled[p] ? "gray90" : color, pass.name, passTypeName(pass.type));
    }
    for (u32 r = 0; r < im.resources.size(); ++r) {
        const auto& res = im.resources[r];
        std::string extra;
        if (res.texture) {
            extra = std::format("{}x{} {}", res.textureDesc.width, res.textureDesc.height, formatName(res.textureDesc.format));
        } else {
            extra = std::format("{} B", res.bufferDesc.size);
        }
        const i32 slot = r < im.plan.resources.size() ? im.plan.resources[r].aliasSlot : -1;
        s << std::format("  r{} [shape={}, label=\"{}\\n{}{}\"];\n", r, res.imported ? "doubleoctagon" : "ellipse", res.name,
                         extra, slot >= 0 ? std::format("\\nslot {}", slot) : "");
    }
    for (u32 p = 0; p < im.passes.size(); ++p) {
        for (const auto& a : im.passes[p].accesses) {
            if (isWrite(a.access)) {
                s << std::format("  p{} -> r{} [color=red, label=\"{}\"];\n", p, a.resource, accessName(a.access));
                if (a.preserve) {
                    s << std::format("  r{} -> p{} [style=dotted];\n", a.resource, p);
                }
            } else {
                s << std::format("  r{} -> p{} [label=\"{}\"];\n", a.resource, p, accessName(a.access));
            }
        }
    }
    s << "}\n";
    return s.str();
}

} // namespace ox::rhi
