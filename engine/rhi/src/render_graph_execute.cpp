// Render graph execution: physical resource allocation (with memory aliasing), barrier emission, submission.
#include "device_impl.hpp"
#include "render_graph_impl.hpp"

#include <oxwald/core/assert.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/rhi/format.hpp>
#include <oxwald/rhi/shader_compiler.hpp>
#include <oxwald/rhi/swapchain.hpp>

#include <algorithm>

namespace ox::rhi {

using namespace detail;

namespace detail {
void* tracyZoneBegin(DeviceState& s, VkCommandBuffer cmd, QueueType queue, const std::string& name);
void tracyZoneEnd(void* zone);
BufferHandle createBufferAliased(Device& device, const BufferDesc& desc, VmaAllocation memory);
} // namespace detail

TextureHandle RenderGraph::Impl::textureHandle(u32 id) const {
    const auto& r = resources.at(id);
    if (r.imported) return r.importedTexture;
    return id < physicalTextures.size() ? physicalTextures[id] : TextureHandle{};
}

BufferHandle RenderGraph::Impl::bufferHandle(u32 id) const {
    const auto& r = resources.at(id);
    if (r.imported) return r.importedBuffer;
    return id < physicalBuffers.size() ? physicalBuffers[id] : BufferHandle{};
}

TextureHandle RenderGraph::physicalTexture(RGTexture t) const { return m_impl->textureHandle(t.id); }
BufferHandle RenderGraph::physicalBuffer(RGBuffer b) const { return m_impl->bufferHandle(b.id); }

RGTexture RenderGraph::importTexture(Device& device, TextureHandle texture, Access finalAccess) {
    TextureDesc d = device.desc(texture);
    return importTexture(texture, d, {device.trackedAccess(texture), finalAccess});
}

// --- PassContext -------------------------------------------------------------------------------------------------

TextureHandle PassContext::texture(RGTexture t) const { return m_graph.textureHandle(t.id); }
BufferHandle PassContext::buffer(RGBuffer b) const { return m_graph.bufferHandle(b.id); }
u32 PassContext::sampledIndex(RGTexture t) const { return device.sampledIndex(texture(t)); }
u32 PassContext::storageIndex(RGTexture t, u32 mip) const { return device.storageIndex(texture(t), mip); }
VkDeviceAddress PassContext::address(RGBuffer b) const { return device.address(buffer(b)); }
VkExtent2D PassContext::extent(RGTexture t, u32 mip) const {
    const TextureDesc& d = m_graph.resources.at(t.id).textureDesc;
    return {std::max(1u, d.width >> mip), std::max(1u, d.height >> mip)};
}

// --- physical resources ------------------------------------------------------------------------------------------

void RenderGraph::releaseResources(Device& device) {
    Impl& g = *m_impl;
    for (TextureHandle t : g.physicalTextures) {
        if (t) device.destroy(t);
    }
    for (BufferHandle b : g.physicalBuffers) {
        if (b) device.destroy(b);
    }
    DeviceState& s = device.state();
    for (void* mem : g.slotMemory) {
        if (mem) {
            auto alloc = static_cast<VmaAllocation>(mem);
            s.retire([&s, alloc] { vmaFreeMemory(s.allocator, alloc); });
        }
    }
    g.physicalTextures.clear();
    g.physicalBuffers.clear();
    g.slotMemory.clear();
    g.physicalHash = 0;
    g.physicalDevice = nullptr;
}

namespace {

// Physical resources depend only on the transient layout (alias slots + descs), not on import states etc.
u64 physicalLayoutHash(const RenderGraph::Impl& g) {
    u64 h = 0xcbf29ce484222325ull;
    auto mix = [&](const auto& v) { h = hashBytes(&v, sizeof(v), h); };
    for (const RGAliasSlot& slot : g.plan.aliasSlots) {
        mix(slot.size);
        mix(slot.memoryTypeBits);
        for (u32 r : slot.resources) {
            const RGResourceDecl& d = g.resources[r];
            const RGResourcePlan& rp = g.plan.resources[r];
            mix(r);
            if (d.texture) {
                const TextureDesc& t = d.textureDesc;
                mix(t.type), mix(t.format), mix(t.width), mix(t.height), mix(t.depth), mix(t.mipLevels);
                mix(t.arrayLayers), mix(t.samples), mix(t.usage), mix(rp.derivedTextureUsage);
                h = hashBytes(t.name.data(), t.name.size(), h);
            } else {
                mix(d.bufferDesc.size), mix(d.bufferDesc.usage), mix(d.bufferDesc.memory), mix(rp.derivedBufferUsage);
            }
        }
    }
    return h | 1; // never 0 (= "nothing allocated")
}

void ensurePhysical(RenderGraph& graph, RenderGraph::Impl& g, Device& device) {
    const u64 layout = physicalLayoutHash(g);
    if (g.physicalDevice == &device && g.physicalHash == layout) return;
    if (g.physicalDevice) graph.releaseResources(*g.physicalDevice);
    DeviceState& s = device.state();
    const RenderGraphPlan& plan = g.plan;
    g.physicalTextures.assign(g.resources.size(), {});
    g.physicalBuffers.assign(g.resources.size(), {});
    g.slotMemory.assign(plan.aliasSlots.size(), nullptr);
    for (u32 si = 0; si < plan.aliasSlots.size(); ++si) {
        const RGAliasSlot& slot = plan.aliasSlots[si];
        VmaAllocation memory = nullptr;
        if (slot.resources.size() > 1) {
            VkMemoryRequirements req{slot.size, slot.alignment, slot.memoryTypeBits};
            VmaAllocationCreateInfo ai{};
            ai.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            OX_VK_CHECK(vmaAllocateMemory(s.allocator, &req, &ai, &memory, nullptr));
            vmaSetAllocationName(s.allocator, memory, std::format("rendergraph.slot{}", si).c_str());
            g.slotMemory[si] = memory;
        }
        for (u32 r : slot.resources) {
            const RGResourceDecl& decl = g.resources[r];
            const RGResourcePlan& rp = plan.resources[r];
            if (decl.texture) {
                TextureDesc d = decl.textureDesc;
                d.usage |= rp.derivedTextureUsage;
                g.physicalTextures[r] = memory ? createTextureAliased(device, d, memory) : device.createTexture(d);
            } else {
                BufferDesc d = decl.bufferDesc;
                d.usage |= rp.derivedBufferUsage;
                g.physicalBuffers[r] = memory ? createBufferAliased(device, d, memory) : device.createBuffer(d);
            }
        }
    }
    g.physicalDevice = &device;
    g.physicalHash = layout;
}

void emitBarriers(RenderGraph::Impl& g, Device& device, CommandList& cmd, const std::vector<RGBarrier>& barriers) {
    if (barriers.empty()) return;
    DeviceState& s = device.state();
    std::vector<VkImageMemoryBarrier2> images;
    std::vector<VkBufferMemoryBarrier2> buffers;
    for (const RGBarrier& b : barriers) {
        if (b.texture) {
            const TextureRecord* t = s.textures.get(g.textureHandle(b.resource));
            if (!t) continue;
            VkImageMemoryBarrier2 ib = makeImageBarrier(s, *t, b.srcStages, b.srcAccessMask, b.dstStages, b.dstAccessMask,
                                                        b.oldLayout, b.newLayout, cmd.queue());
            ib.srcQueueFamilyIndex = b.srcFamily;
            ib.dstQueueFamilyIndex = b.dstFamily;
            images.push_back(ib);
        } else {
            const BufferRecord* br = s.buffers.get(g.bufferHandle(b.resource));
            if (!br) continue;
            VkBufferMemoryBarrier2 bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            bb.srcStageMask = s.sanitizeStages(b.srcStages, cmd.queue());
            bb.srcAccessMask = s.sanitizeAccess(b.srcAccessMask);
            bb.dstStageMask = s.sanitizeStages(b.dstStages, cmd.queue());
            bb.dstAccessMask = s.sanitizeAccess(b.dstAccessMask);
            bb.srcQueueFamilyIndex = b.srcFamily;
            bb.dstQueueFamilyIndex = b.dstFamily;
            bb.buffer = br->buffer;
            bb.size = VK_WHOLE_SIZE;
            buffers.push_back(bb);
        }
    }
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = u32(images.size());
    di.pImageMemoryBarriers = images.data();
    di.bufferMemoryBarrierCount = u32(buffers.size());
    di.pBufferMemoryBarriers = buffers.data();
    vkCmdPipelineBarrier2(cmd.vk(), &di);
}

RGCompileOptions compileOptionsFor(Device& device, bool async) {
    RGCompileOptions o;
    o.asyncCompute = async && device.caps().asyncComputeQueue;
    for (u32 q = 0; q < kQueueTypeCount; ++q) o.queueFamilies[q] = device.queueFamily(QueueType(q));
    o.textureRequirements = [&device](const TextureDesc& d) { return device.memoryRequirements(d); };
    o.bufferRequirements = [&device](const BufferDesc& d) { return device.memoryRequirements(d); };
    return o;
}

void updateTracking(RenderGraph::Impl& g, Device& device) {
    for (u32 r = 0; r < g.resources.size(); ++r) {
        const RGResourceDecl& d = g.resources[r];
        if (!d.texture) continue;
        const RGResourcePlan& rp = g.plan.resources[r];
        if (!rp.used) continue;
        const TextureHandle h = g.textureHandle(r);
        if (h) device.setTrackedAccess(h, rp.finalAccess);
    }
}

} // namespace

void RenderGraph::Impl::recordPass(CommandList& cmd, Device& device, const RGPlannedPass& planned, bool timestamps,
                                   std::string_view timestampPrefix) {
    const RGPassDecl& decl = passes[planned.pass];
    static constexpr f32 kColors[4][4] = {{0.4f, 0.8f, 0.4f, 1.f}, {0.4f, 0.6f, 1.f, 1.f}, {1.f, 0.8f, 0.3f, 1.f}, {0.9f, 0.4f, 0.9f, 1.f}};
    cmd.beginLabel(decl.name, kColors[u32(decl.type) & 3]);
    if (timestamps) {
        if (timestampPrefix.empty()) cmd.beginTimestamp(decl.name);
        else cmd.beginTimestamp(std::string(timestampPrefix) + decl.name);
    }
    void* zone = tracyZoneBegin(device.state(), cmd.vk(), cmd.queue(), decl.name);

    emitBarriers(*this, device, cmd, planned.before);
    const bool rendering = !decl.colors.empty() || decl.depth.has_value();
    SecondaryRenderingInfo secondaryInfo;
    if (rendering) {
        RenderingDesc rd;
        rd.secondaryContents = decl.secondary;
        for (const RGAttachmentDecl& a : decl.colors) {
            ColorAttachment c;
            c.texture = textureHandle(a.resource);
            c.mip = a.mip;
            c.layer = a.layer;
            c.load = a.load;
            c.clear = a.clear;
            if (a.resolveTarget != ~0u) c.resolve = textureHandle(a.resolveTarget);
            rd.colors.push_back(c);
        }
        if (decl.depth) {
            DepthAttachment d;
            d.texture = textureHandle(decl.depth->resource);
            d.mip = decl.depth->mip;
            d.layer = decl.depth->layer;
            d.load = decl.depth->load;
            d.clear = decl.depth->clearDepth;
            d.readOnly = decl.depth->readOnly;
            d.store = d.readOnly ? VK_ATTACHMENT_STORE_OP_NONE : VK_ATTACHMENT_STORE_OP_STORE;
            rd.depth = d;
        }
        cmd.beginRendering(rd);
        if (decl.secondary) {
            for (const ColorAttachment& c : rd.colors) secondaryInfo.colorFormats.push_back(device.desc(c.texture).format);
            VkExtent2D extent{0, 0};
            if (!rd.colors.empty()) {
                const TextureDesc& t = device.desc(rd.colors[0].texture);
                extent = {std::max(1u, t.width >> rd.colors[0].mip), std::max(1u, t.height >> rd.colors[0].mip)};
            }
            if (rd.depth) {
                const TextureDesc& t = device.desc(rd.depth->texture);
                const FormatInfo fi = formatInfo(t.format);
                if (fi.depth) secondaryInfo.depthFormat = t.format;
                if (fi.stencil) secondaryInfo.stencilFormat = t.format;
                if (extent.width == 0) extent = {std::max(1u, t.width >> rd.depth->mip), std::max(1u, t.height >> rd.depth->mip)};
            }
            secondaryInfo.area = {{0, 0}, extent};
        }
    }
    if (decl.fn) {
        PassContext ctx(cmd, device, *this);
        if (decl.secondary && rendering) ctx.secondaryRendering = &secondaryInfo;
        decl.fn(ctx);
    }
    if (rendering) cmd.endRendering();
    emitBarriers(*this, device, cmd, planned.after);

    tracyZoneEnd(zone);
    if (timestamps) cmd.endTimestamp();
    cmd.endLabel();
}

void RenderGraph::execute(CommandList& cmd, std::string_view timestampPrefix) {
    Device& device = cmd.device();
    compile(compileOptionsFor(device, false));
    ensurePhysical(*this, *m_impl, device);
    for (const RGPlannedPass& p : m_impl->plan.passes) {
        m_impl->recordPass(cmd, device, p, true, timestampPrefix);
    }
    updateTracking(*m_impl, device);
}

void RenderGraph::execute(Device& device, const RGExecuteOptions& options) {
    Impl& g = *m_impl;
    compile(compileOptionsFor(device, options.asyncCompute));
    ensurePhysical(*this, g, device);
    const RenderGraphPlan& plan = g.plan;

    i32 firstGraphics = -1, lastGraphics = -1;
    for (u32 b = 0; b < plan.batches.size(); ++b) {
        if (plan.batches[b].queue == QueueType::Graphics) {
            if (firstGraphics < 0) firstGraphics = i32(b);
            lastGraphics = i32(b);
        }
    }
    std::vector<TimelinePoint> points(plan.batches.size());
    std::array<bool, kQueueTypeCount> queueStarted{};
    std::vector<TimelinePoint> lastPoints;
    std::vector<CommandList*> recorded(plan.batches.size(), nullptr);
    u32 submitted = 0;
    // Submits the recorded batches [submitted, end) in plan order.
    auto submitUpTo = [&](u32 end) {
        for (; submitted < end; ++submitted) {
            const u32 b = submitted;
            const RGBatch& batch = plan.batches[b];
            CommandList& cmd = *recorded[b];
            SubmitInfo si;
            for (u32 w : batch.waitBatches) si.waits.push_back(points[w]);
            if (b == 0) si.waits.insert(si.waits.end(), options.waits.begin(), options.waits.end());
            if (!queueStarted[u32(batch.queue)]) {
                // Transient memory is reused across frames: order against the previous execution on other queues.
                for (const TimelinePoint& p : g.lastExecution) {
                    if (p.queue != batch.queue) si.waits.push_back(p);
                }
                queueStarted[u32(batch.queue)] = true;
            }
            if (options.swapchain && i32(b) == firstGraphics) {
                si.waitSemaphores.push_back({options.swapchain->acquireSemaphore(), 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT});
            }
            if (options.swapchain && i32(b) == lastGraphics) {
                si.signalSemaphores.push_back(options.swapchain->presentSemaphore());
            }
            points[b] = device.submit(cmd, si);
        }
    };
    for (u32 b = 0; b < plan.batches.size(); ++b) {
        const RGBatch& batch = plan.batches[b];
        CommandList& cmd = device.commandList(batch.queue, std::format("rendergraph.batch{}", b));
        for (u32 pi : batch.passes) g.recordPass(cmd, device, plan.passes[pi], options.timestamps, options.timestampPrefix);
        recorded[b] = &cmd;
        // An async batch is held back until the following graphics batch is recorded and both are submitted back to
        // back: submitted on its own it would be finished by the time the CPU has recorded the graphics passes that
        // are meant to overlap with it.
        if (batch.queue == QueueType::Graphics || b + 1 == plan.batches.size()) submitUpTo(b + 1);
    }
    if (options.swapchain && firstGraphics < 0) {
        CommandList& cmd = device.commandList(QueueType::Graphics, "rendergraph.present");
        SubmitInfo si;
        si.swapchain = options.swapchain;
        device.submit(cmd, si);
    }
    for (u32 q = 0; q < kQueueTypeCount; ++q) {
        for (i32 b = i32(plan.batches.size()) - 1; b >= 0; --b) {
            if (u32(plan.batches[u32(b)].queue) == q) {
                lastPoints.push_back(points[u32(b)]);
                break;
            }
        }
    }
    if (!lastPoints.empty()) g.lastExecution = lastPoints;
    updateTracking(g, device);
}

} // namespace ox::rhi
