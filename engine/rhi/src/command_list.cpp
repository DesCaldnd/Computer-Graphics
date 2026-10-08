#include "device_impl.hpp"

#include <oxwald/core/assert.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/rhi/format.hpp>

#include <algorithm>
#include <bit>

namespace ox::rhi {

using namespace detail;

namespace {

DeviceState& st(Device& d) { return d.state(); }

VkExtent3D mipExtent(const TextureDesc& d, u32 mip) {
    return {std::max(1u, d.width >> mip), std::max(1u, d.height >> mip), std::max(1u, d.depth >> mip)};
}

VkImageAspectFlags copyAspect(const TextureRecord& t) {
    // Copies address one aspect at a time; depth is the common case.
    return formatInfo(t.desc.format).depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : t.aspect;
}

void dependency(VkCommandBuffer cmd, const VkImageMemoryBarrier2* images, u32 imageCount, const VkBufferMemoryBarrier2* buffers,
                u32 bufferCount, const VkMemoryBarrier2* memory, u32 memoryCount) {
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = imageCount;
    di.pImageMemoryBarriers = images;
    di.bufferMemoryBarrierCount = bufferCount;
    di.pBufferMemoryBarriers = buffers;
    di.memoryBarrierCount = memoryCount;
    di.pMemoryBarriers = memory;
    vkCmdPipelineBarrier2(cmd, &di);
}

} // namespace

// --- synchronization ---------------------------------------------------------------------------------------------

void CommandList::transition(TextureHandle texture, Access newAccess, bool discard) {
    DeviceState& s = st(*m_device);
    TextureRecord& t = s.textures.at(texture);
    const Access old = t.tracked;
    const AccessInfo src = accessInfo(old);
    const AccessInfo dst = accessInfo(newAccess);
    if (old == newAccess && !dst.write && !discard) return;
    VkPipelineStageFlags2 srcStages = src.stages;
    if (old == Access::Undefined || old == Access::Present) srcStages = dst.stages; // chain after acquire / earlier waits
    VkImageMemoryBarrier2 b = makeImageBarrier(s, t, srcStages, src.write ? src.access : 0, dst.stages, dst.access,
                                               discard ? VK_IMAGE_LAYOUT_UNDEFINED : src.layout, dst.layout, m_queue);
    if (b.srcStageMask == 0 && b.dstStageMask == 0) b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    dependency(m_cmd, &b, 1, nullptr, 0, nullptr, 0);
    t.tracked = newAccess;
}

void CommandList::textureBarrier(TextureHandle texture, Access srcA, Access dstA, TextureSubresource range, bool discard) {
    DeviceState& s = st(*m_device);
    TextureRecord& t = s.textures.at(texture);
    const AccessInfo src = accessInfo(srcA);
    const AccessInfo dst = accessInfo(dstA);
    VkPipelineStageFlags2 srcStages = srcA == Access::Undefined ? dst.stages : src.stages;
    VkImageMemoryBarrier2 b = makeImageBarrier(s, t, srcStages, src.write ? src.access : 0, dst.stages, dst.access,
                                               discard ? VK_IMAGE_LAYOUT_UNDEFINED : src.layout, dst.layout, m_queue, range);
    dependency(m_cmd, &b, 1, nullptr, 0, nullptr, 0);
    if (range == TextureSubresource{}) t.tracked = dstA;
}

void CommandList::bufferBarrier(BufferHandle buffer, Access srcA, Access dstA, u64 offset, u64 size) {
    DeviceState& s = st(*m_device);
    const AccessInfo src = accessInfo(srcA);
    const AccessInfo dst = accessInfo(dstA);
    VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    b.srcStageMask = s.sanitizeStages(src.stages, m_queue);
    b.srcAccessMask = s.sanitizeAccess(src.write ? src.access : 0);
    b.dstStageMask = s.sanitizeStages(dst.stages, m_queue);
    b.dstAccessMask = s.sanitizeAccess(dst.access);
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = s.buffers.at(buffer).buffer;
    b.offset = offset;
    b.size = size;
    dependency(m_cmd, nullptr, 0, &b, 1, nullptr, 0);
}

void CommandList::memoryBarrier(Access srcA, Access dstA) {
    DeviceState& s = st(*m_device);
    const AccessInfo src = accessInfo(srcA);
    const AccessInfo dst = accessInfo(dstA);
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = s.sanitizeStages(src.stages ? src.stages : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_queue);
    b.srcAccessMask = s.sanitizeAccess(src.write ? src.access : 0);
    b.dstStageMask = s.sanitizeStages(dst.stages ? dst.stages : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_queue);
    b.dstAccessMask = s.sanitizeAccess(dst.access);
    dependency(m_cmd, nullptr, 0, nullptr, 0, &b, 1);
}

void CommandList::pipelineBarrier(const VkDependencyInfo& info) { vkCmdPipelineBarrier2(m_cmd, &info); }

// --- rendering ---------------------------------------------------------------------------------------------------

void CommandList::beginRendering(const RenderingDesc& desc) {
    DeviceState& s = st(*m_device);
    std::vector<VkRenderingAttachmentInfo> colors;
    colors.reserve(desc.colors.size());
    VkExtent2D extent{0, 0};
    const bool layered = desc.layerCount > 1 || desc.viewMask != 0;
    const u32 layers = desc.viewMask ? 32u - u32(std::countl_zero(desc.viewMask)) : desc.layerCount;
    auto attachmentView = [&](TextureRecord& t, u32 mip, u32 layer, VkImageAspectFlags aspect) {
        TextureViewDesc vd;
        vd.range = {mip, 1, layer, layered ? layers : 1u};
        vd.type = layered ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        if (t.desc.type == TextureType::Tex2D && t.desc.arrayLayers == 1 && mip == 0 && !layered && t.desc.mipLevels == 1 &&
            aspect == t.aspect && !formatInfo(t.desc.format).depth) {
            return t.defaultView;
        }
        return textureView(s, t, vd, aspect);
    };
    for (const ColorAttachment& c : desc.colors) {
        TextureRecord& t = s.textures.at(c.texture);
        VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        a.imageView = attachmentView(t, c.mip, c.layer, t.aspect);
        a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        a.loadOp = c.load;
        a.storeOp = c.store;
        std::copy(c.clear.f.begin(), c.clear.f.end(), a.clearValue.color.float32);
        if (c.resolve) {
            TextureRecord& r = s.textures.at(c.resolve);
            a.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
            a.resolveImageView = attachmentView(r, 0, 0, r.aspect);
            a.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        colors.push_back(a);
        if (extent.width == 0) {
            const VkExtent3D e = mipExtent(t.desc, c.mip);
            extent = {e.width, e.height};
        }
    }
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo stencil{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    bool hasDepth = false, hasStencil = false;
    if (desc.depth) {
        TextureRecord& t = s.textures.at(desc.depth->texture);
        const FormatInfo fi = formatInfo(t.desc.format);
        depth.imageView = attachmentView(t, desc.depth->mip, desc.depth->layer, t.aspect);
        depth.imageLayout = desc.depth->readOnly ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                 : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp = desc.depth->load;
        depth.storeOp = desc.depth->store;
        depth.clearValue.depthStencil = {desc.depth->clear.depth, desc.depth->clear.stencil};
        hasDepth = fi.depth;
        if (fi.stencil) {
            stencil = depth;
            hasStencil = true;
        }
        if (extent.width == 0) {
            const VkExtent3D e = mipExtent(t.desc, desc.depth->mip);
            extent = {e.width, e.height};
        }
    }
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = desc.area;
    if (ri.renderArea.extent.width == 0) ri.renderArea = {{0, 0}, extent};
    ri.layerCount = desc.viewMask ? 1 : desc.layerCount;
    ri.viewMask = desc.viewMask;
    ri.colorAttachmentCount = u32(colors.size());
    ri.pColorAttachments = colors.data();
    ri.pDepthAttachment = hasDepth ? &depth : nullptr;
    ri.pStencilAttachment = hasStencil ? &stencil : nullptr;
    if (desc.secondaryContents) ri.flags |= VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;
    vkCmdBeginRendering(m_cmd, &ri);
    if (desc.setViewportAndScissor && !desc.secondaryContents) {
        setViewport(f32(ri.renderArea.offset.x), f32(ri.renderArea.offset.y), f32(ri.renderArea.extent.width),
                    f32(ri.renderArea.extent.height));
        setScissor(ri.renderArea.offset.x, ri.renderArea.offset.y, ri.renderArea.extent.width, ri.renderArea.extent.height);
    }
}

void CommandList::endRendering() { vkCmdEndRendering(m_cmd); }

void CommandList::executeSecondary(std::span<CommandList* const> lists) {
    if (lists.empty()) return;
    VkCommandBuffer stack[64];
    std::vector<VkCommandBuffer> heap;
    VkCommandBuffer* cbs = stack;
    if (lists.size() > std::size(stack)) {
        heap.resize(lists.size());
        cbs = heap.data();
    }
    for (usize i = 0; i < lists.size(); ++i) {
        OX_ASSERT(lists[i]->m_secondary, "executeSecondary: not a secondary command list");
        cbs[i] = lists[i]->vk();
    }
    vkCmdExecuteCommands(m_cmd, u32(lists.size()), cbs);
}

void CommandList::end() { OX_VK_CHECK(vkEndCommandBuffer(m_cmd)); }

void CommandList::setViewport(f32 x, f32 y, f32 width, f32 height, f32 minDepth, f32 maxDepth) {
    const VkViewport vp{x, y, width, height, minDepth, maxDepth};
    vkCmdSetViewport(m_cmd, 0, 1, &vp);
}

void CommandList::setScissor(i32 x, i32 y, u32 width, u32 height) {
    const VkRect2D r{{x, y}, {width, height}};
    vkCmdSetScissor(m_cmd, 0, 1, &r);
}

void CommandList::setDepthBias(f32 constant, f32 clamp, f32 slope) { vkCmdSetDepthBias(m_cmd, constant, clamp, slope); }
void CommandList::setStencilReference(u32 reference) {
    vkCmdSetStencilReference(m_cmd, VK_STENCIL_FACE_FRONT_AND_BACK, reference);
}

void CommandList::bindPipeline(PipelineHandle pipeline) {
    DeviceState& s = st(*m_device);
    PipelineRecord& p = s.pipelines.at(pipeline);
    m_boundPipeline = pipeline;
    if (!p.pipeline) {
        return; // broken shader: draws/dispatches are skipped until it compiles
    }
    const VkPipelineBindPoint bp = p.kind == PipelineKind::Compute      ? VK_PIPELINE_BIND_POINT_COMPUTE
                                   : p.kind == PipelineKind::RayTracing ? VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR
                                                                        : VK_PIPELINE_BIND_POINT_GRAPHICS;
    vkCmdBindPipeline(m_cmd, bp, p.pipeline);
    vkCmdBindDescriptorSets(m_cmd, bp, s.pipelineLayout, 0, 1, &s.bindlessSet, 0, nullptr);
}

namespace {
bool pipelineReady(Device& d, PipelineHandle h) {
    const PipelineRecord* p = d.state().pipelines.get(h);
    return p && p->pipeline;
}
} // namespace

void CommandList::pushConstants(const void* data, u32 size, u32 offset) {
    OX_ASSERT(offset + size <= kMaxPushConstantSize, "push constants exceed {} bytes", kMaxPushConstantSize);
    vkCmdPushConstants(m_cmd, st(*m_device).pipelineLayout, VK_SHADER_STAGE_ALL, offset, size, data);
}

void CommandList::bindVertexBuffer(u32 binding, BufferHandle buffer, u64 offset) {
    VkBuffer b = st(*m_device).buffers.at(buffer).buffer;
    vkCmdBindVertexBuffers(m_cmd, binding, 1, &b, &offset);
}

void CommandList::bindIndexBuffer(BufferHandle buffer, u64 offset, VkIndexType type) {
    vkCmdBindIndexBuffer(m_cmd, st(*m_device).buffers.at(buffer).buffer, offset, type);
}

void CommandList::draw(u32 vertexCount, u32 instanceCount, u32 firstVertex, u32 firstInstance) {
    if (!pipelineReady(*m_device, m_boundPipeline)) return;
    vkCmdDraw(m_cmd, vertexCount, instanceCount, firstVertex, firstInstance);
}

void CommandList::drawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex, i32 vertexOffset, u32 firstInstance) {
    if (!pipelineReady(*m_device, m_boundPipeline)) return;
    vkCmdDrawIndexed(m_cmd, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void CommandList::drawIndirect(BufferHandle args, u64 offset, u32 drawCount, u32 stride) {
    if (!pipelineReady(*m_device, m_boundPipeline)) return;
    vkCmdDrawIndirect(m_cmd, st(*m_device).buffers.at(args).buffer, offset, drawCount, stride);
}

void CommandList::drawIndexedIndirect(BufferHandle args, u64 offset, u32 drawCount, u32 stride) {
    if (!pipelineReady(*m_device, m_boundPipeline)) return;
    vkCmdDrawIndexedIndirect(m_cmd, st(*m_device).buffers.at(args).buffer, offset, drawCount, stride);
}

void CommandList::drawIndirectCount(BufferHandle args, u64 offset, BufferHandle count, u64 countOffset, u32 maxDraws,
                                    u32 stride) {
    DeviceState& s = st(*m_device);
    OX_ASSERT(s.caps.drawIndirectCount, "drawIndirectCount is not supported by this device");
    if (!pipelineReady(*m_device, m_boundPipeline)) return;
    vkCmdDrawIndirectCount(m_cmd, s.buffers.at(args).buffer, offset, s.buffers.at(count).buffer, countOffset, maxDraws, stride);
}

void CommandList::drawIndexedIndirectCount(BufferHandle args, u64 offset, BufferHandle count, u64 countOffset,
                                           u32 maxDraws, u32 stride) {
    DeviceState& s = st(*m_device);
    OX_ASSERT(s.caps.drawIndirectCount, "drawIndirectCount is not supported by this device");
    if (!pipelineReady(*m_device, m_boundPipeline)) return;
    vkCmdDrawIndexedIndirectCount(m_cmd, s.buffers.at(args).buffer, offset, s.buffers.at(count).buffer, countOffset,
                                  maxDraws, stride);
}

void CommandList::drawMeshTasks(u32 x, u32 y, u32 z) {
    OX_ASSERT(st(*m_device).caps.meshShader, "mesh shaders are not supported by this device");
    if (!pipelineReady(*m_device, m_boundPipeline)) return;
    vkCmdDrawMeshTasksEXT(m_cmd, x, y, z);
}

void CommandList::drawMeshTasksIndirect(BufferHandle args, u64 offset, u32 drawCount, u32 stride) {
    DeviceState& s = st(*m_device);
    OX_ASSERT(s.caps.meshShader, "mesh shaders are not supported by this device");
    if (!pipelineReady(*m_device, m_boundPipeline)) return;
    const BufferRecord* b = s.buffers.get(args);
    if (!b) return;
    vkCmdDrawMeshTasksIndirectEXT(m_cmd, b->buffer, offset, drawCount, stride);
}

// --- compute / ray tracing ---------------------------------------------------------------------------------------

void CommandList::dispatch(u32 x, u32 y, u32 z) {
    if (!pipelineReady(*m_device, m_boundPipeline)) return;
    vkCmdDispatch(m_cmd, x, y, z);
}

void CommandList::dispatchThreads(u32 x, u32 y, u32 z) {
    const PipelineRecord& p = st(*m_device).pipelines.at(m_boundPipeline);
    auto groups = [](u32 n, u32 local) { return (n + std::max(1u, local) - 1) / std::max(1u, local); };
    dispatch(groups(x, p.localSize[0]), groups(y, p.localSize[1]), groups(z, p.localSize[2]));
}

void CommandList::dispatchIndirect(BufferHandle args, u64 offset) {
    if (!pipelineReady(*m_device, m_boundPipeline)) return;
    vkCmdDispatchIndirect(m_cmd, st(*m_device).buffers.at(args).buffer, offset);
}

void CommandList::traceRays(u32 width, u32 height, u32 depth) {
    DeviceState& s = st(*m_device);
    OX_ASSERT(s.caps.rayTracingPipeline, "ray tracing pipelines are not supported by this device");
    if (!pipelineReady(*m_device, m_boundPipeline)) return;
    const PipelineRecord& p = s.pipelines.at(m_boundPipeline);
    vkCmdTraceRaysKHR(m_cmd, &p.regions[0], &p.regions[1], &p.regions[2], &p.regions[3], width, height, depth);
}

// --- transfer ----------------------------------------------------------------------------------------------------

void CommandList::copyBuffer(BufferHandle src, BufferHandle dst, u64 size, u64 srcOffset, u64 dstOffset) {
    DeviceState& s = st(*m_device);
    const VkBufferCopy region{srcOffset, dstOffset, size};
    vkCmdCopyBuffer(m_cmd, s.buffers.at(src).buffer, s.buffers.at(dst).buffer, 1, &region);
}

void CommandList::copyBufferToTexture(BufferHandle src, u64 srcOffset, TextureHandle dst, u32 mip, u32 layer, u32 layerCount) {
    DeviceState& s = st(*m_device);
    const TextureRecord& t = s.textures.at(dst);
    VkBufferImageCopy r{};
    r.bufferOffset = srcOffset;
    r.imageSubresource = {copyAspect(t), mip, layer, layerCount};
    r.imageExtent = mipExtent(t.desc, mip);
    vkCmdCopyBufferToImage(m_cmd, s.buffers.at(src).buffer, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
}

void CommandList::copyTextureToBuffer(TextureHandle src, u32 mip, u32 layer, BufferHandle dst, u64 dstOffset, u32 layerCount) {
    DeviceState& s = st(*m_device);
    const TextureRecord& t = s.textures.at(src);
    VkBufferImageCopy r{};
    r.bufferOffset = dstOffset;
    r.imageSubresource = {copyAspect(t), mip, layer, layerCount};
    r.imageExtent = mipExtent(t.desc, mip);
    vkCmdCopyImageToBuffer(m_cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.buffers.at(dst).buffer, 1, &r);
}

void CommandList::copyTexture(TextureHandle src, TextureHandle dst, u32 mip, u32 layer) {
    DeviceState& s = st(*m_device);
    const TextureRecord& a = s.textures.at(src);
    const TextureRecord& b = s.textures.at(dst);
    VkImageCopy r{};
    r.srcSubresource = {copyAspect(a), mip, layer, 1};
    r.dstSubresource = {copyAspect(b), mip, layer, 1};
    r.extent = mipExtent(a.desc, mip);
    vkCmdCopyImage(m_cmd, a.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
}

void CommandList::blitTexture(TextureHandle src, u32 srcMip, u32 srcLayer, TextureHandle dst, u32 dstMip, u32 dstLayer,
                              VkFilter filter) {
    DeviceState& s = st(*m_device);
    const TextureRecord& a = s.textures.at(src);
    const TextureRecord& b = s.textures.at(dst);
    const VkExtent3D ea = mipExtent(a.desc, srcMip);
    const VkExtent3D eb = mipExtent(b.desc, dstMip);
    VkImageBlit r{};
    r.srcSubresource = {a.aspect, srcMip, srcLayer, 1};
    r.srcOffsets[1] = {i32(ea.width), i32(ea.height), i32(ea.depth)};
    r.dstSubresource = {b.aspect, dstMip, dstLayer, 1};
    r.dstOffsets[1] = {i32(eb.width), i32(eb.height), i32(eb.depth)};
    vkCmdBlitImage(m_cmd, a.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r,
                   filter);
}

void CommandList::clearTexture(TextureHandle texture, const ClearColor& color) {
    DeviceState& s = st(*m_device);
    const TextureRecord& t = s.textures.at(texture);
    VkClearColorValue v{};
    std::copy(color.f.begin(), color.f.end(), v.float32);
    const VkImageSubresourceRange range = fullRange(t);
    vkCmdClearColorImage(m_cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range);
}

void CommandList::clearDepthStencil(TextureHandle texture, const ClearDepthStencil& value) {
    DeviceState& s = st(*m_device);
    const TextureRecord& t = s.textures.at(texture);
    const VkClearDepthStencilValue v{value.depth, value.stencil};
    const VkImageSubresourceRange range = fullRange(t);
    vkCmdClearDepthStencilImage(m_cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range);
}

void CommandList::fillBuffer(BufferHandle buffer, u32 value, u64 offset, u64 size) {
    vkCmdFillBuffer(m_cmd, st(*m_device).buffers.at(buffer).buffer, offset, size, value);
}

void CommandList::updateBuffer(BufferHandle buffer, const void* data, u64 size, u64 offset) {
    OX_ASSERT(size <= 65536 && size % 4 == 0, "updateBuffer: size must be ≤ 64 KiB and a multiple of 4");
    vkCmdUpdateBuffer(m_cmd, st(*m_device).buffers.at(buffer).buffer, offset, size, data);
}

void CommandList::generateMipmaps(TextureHandle texture, Access finalAccess) {
    DeviceState& s = st(*m_device);
    TextureRecord& t = s.textures.at(texture);
    const u32 mips = t.desc.mipLevels;
    const u32 layers = t.desc.arrayLayers;
    // mip 0 -> TransferRead, other mips -> TransferWrite (discard).
    textureBarrier(texture, t.tracked, Access::TransferRead, {0, 1, 0, ~0u});
    if (mips > 1) textureBarrier(texture, Access::Undefined, Access::TransferWrite, {1, ~0u, 0, ~0u}, true);
    for (u32 m = 1; m < mips; ++m) {
        for (u32 l = 0; l < layers; ++l) {
            blitTexture(texture, m - 1, l, texture, m, l, VK_FILTER_LINEAR);
        }
        textureBarrier(texture, Access::TransferWrite, Access::TransferRead, {m, 1, 0, ~0u});
    }
    t.tracked = Access::TransferRead;
    transition(texture, finalAccess);
}

// --- debugging & profiling ---------------------------------------------------------------------------------------

void CommandList::beginLabel(std::string_view name, const f32 color[4]) {
    if (!st(*m_device).debugUtils || !vkCmdBeginDebugUtilsLabelEXT) return;
    std::string n(name);
    VkDebugUtilsLabelEXT l{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
    l.pLabelName = n.c_str();
    if (color) std::copy(color, color + 4, l.color);
    vkCmdBeginDebugUtilsLabelEXT(m_cmd, &l);
}

void CommandList::endLabel() {
    if (!st(*m_device).debugUtils || !vkCmdEndDebugUtilsLabelEXT) return;
    vkCmdEndDebugUtilsLabelEXT(m_cmd);
}

void CommandList::insertLabel(std::string_view name) {
    if (!st(*m_device).debugUtils || !vkCmdInsertDebugUtilsLabelEXT) return;
    std::string n(name);
    VkDebugUtilsLabelEXT l{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
    l.pLabelName = n.c_str();
    vkCmdInsertDebugUtilsLabelEXT(m_cmd, &l);
}

void CommandList::beginTimestamp(std::string_view name) {
    DeviceState& s = st(*m_device);
    if (m_frameSlot < 0 || !s.caps.timestampQueries || !s.queue(m_queue).timestamps) {
        m_openTimestamps.push_back(~0u);
        return;
    }
    FrameContext& f = s.frames[u32(m_frameSlot)];
    if (f.queryCount + 2 > 512) {
        m_openTimestamps.push_back(~0u);
        return;
    }
    const u32 q = f.queryCount++;
    vkCmdWriteTimestamp2(m_cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, f.queryPool, q);
    f.scopes.push_back({std::string(name), u32(m_openTimestamps.size()), q, ~0u, u8(m_queue)});
    m_openTimestamps.push_back(u32(f.scopes.size() - 1));
}

void CommandList::endTimestamp() {
    OX_ASSERT(!m_openTimestamps.empty(), "endTimestamp without beginTimestamp");
    const u32 scope = m_openTimestamps.back();
    m_openTimestamps.pop_back();
    if (scope == ~0u) return;
    DeviceState& s = st(*m_device);
    FrameContext& f = s.frames[u32(m_frameSlot)];
    const u32 q = f.queryCount++;
    vkCmdWriteTimestamp2(m_cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, f.queryPool, q);
    f.scopes[scope].endQuery = q;
}

} // namespace ox::rhi
