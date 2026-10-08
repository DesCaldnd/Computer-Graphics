#include "device_impl.hpp"

#include <oxwald/core/assert.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/rhi/format.hpp>

#include <algorithm>
#include <cstring>

namespace ox::rhi {

using namespace detail;

namespace detail {

u64 submitRaw(DeviceState& s, QueueType queue, std::span<const VkCommandBuffer> cmds, std::vector<VkSemaphoreSubmitInfo> waits,
              std::vector<VkSemaphoreSubmitInfo> signals);

VkBufferUsageFlags toVkBufferUsage(BufferUsage u) {
    VkBufferUsageFlags f = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (any(u & BufferUsage::Vertex)) f |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (any(u & BufferUsage::Index)) f |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (any(u & BufferUsage::Uniform)) f |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (any(u & BufferUsage::Indirect)) f |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    return f;
}

VkImageUsageFlags toVkImageUsage(TextureUsage u, VkFormat format) {
    VkImageUsageFlags f = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (any(u & TextureUsage::Sampled)) f |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (any(u & TextureUsage::Storage)) f |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (any(u & TextureUsage::ColorAttachment)) f |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (any(u & TextureUsage::DepthStencilAttachment)) f |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    (void)format;
    return f;
}

TextureDesc normalizedDesc(TextureDesc d) {
    d.width = std::max(1u, d.width);
    d.height = d.type == TextureType::Tex1D ? 1u : std::max(1u, d.height);
    d.depth = d.type == TextureType::Tex3D ? std::max(1u, d.depth) : 1u;
    if (d.mipLevels == 0) d.mipLevels = fullMipCount(d.width, d.height, d.depth);
    d.arrayLayers = std::max(1u, d.arrayLayers);
    if (d.type == TextureType::Cube && d.arrayLayers % 6 != 0) d.arrayLayers = 6;
    if (d.type == TextureType::Tex3D) d.arrayLayers = 1;
    d.samples = std::max(1u, d.samples);
    return d;
}

VkImageCreateInfo makeImageInfo(const TextureDesc& d) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = d.type == TextureType::Tex1D ? VK_IMAGE_TYPE_1D : d.type == TextureType::Tex3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = d.format;
    ci.extent = {d.width, d.height, d.depth};
    ci.mipLevels = d.mipLevels;
    ci.arrayLayers = d.arrayLayers;
    ci.samples = VkSampleCountFlagBits(d.samples);
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = toVkImageUsage(d.usage, d.format);
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (d.type == TextureType::Cube) ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (d.type == TextureType::Tex3D && any(d.usage & TextureUsage::ColorAttachment)) {
        ci.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    }
    return ci;
}

VkImageSubresourceRange fullRange(const TextureRecord& t, const TextureSubresource& r) {
    VkImageSubresourceRange range{};
    range.aspectMask = t.aspect;
    range.baseMipLevel = r.baseMip;
    range.levelCount = r.mipCount == ~0u ? VK_REMAINING_MIP_LEVELS : r.mipCount;
    range.baseArrayLayer = r.baseLayer;
    range.layerCount = r.layerCount == ~0u ? VK_REMAINING_ARRAY_LAYERS : r.layerCount;
    return range;
}

VkImageMemoryBarrier2 makeImageBarrier(const DeviceState& s, const TextureRecord& t, VkPipelineStageFlags2 srcStages,
                                       VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStages, VkAccessFlags2 dstAccess,
                                       VkImageLayout oldLayout, VkImageLayout newLayout, QueueType queue,
                                       const TextureSubresource& range) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = s.sanitizeStages(srcStages, queue);
    b.srcAccessMask = s.sanitizeAccess(srcAccess);
    b.dstStageMask = s.sanitizeStages(dstStages, queue);
    b.dstAccessMask = s.sanitizeAccess(dstAccess);
    b.oldLayout = oldLayout;
    b.newLayout = newLayout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_GENERAL : newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t.image;
    b.subresourceRange = fullRange(t, range);
    return b;
}

void DeviceState::writeBindlessSampled(u32 index, VkImageView view) {
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = bindlessSet;
    w.dstBinding = 0;
    w.dstArrayElement = index;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
}

void DeviceState::writeBindlessStorage(u32 index, VkImageView view) {
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = bindlessSet;
    w.dstBinding = 1;
    w.dstArrayElement = index;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
}

void DeviceState::writeBindlessSampler(u32 index, VkSampler sampler) {
    VkDescriptorImageInfo ii{sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = bindlessSet;
    w.dstBinding = 2;
    w.dstArrayElement = index;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
}

void createBindless(DeviceState& s) {
    const DeviceCaps& c = s.caps;
    // Leave head-room for other descriptors the implementation may need (e.g. Tracy, Metal argument buffers).
    const u32 sampled = std::max(16u, std::min(s.desc.maxBindlessSampledImages, c.maxBindlessSampledImages));
    const u32 storage = std::max(8u, std::min(s.desc.maxBindlessStorageImages, c.maxBindlessStorageImages));
    const u32 samplers = std::max(8u, std::min(s.desc.maxBindlessSamplers, c.maxBindlessSamplers));
    s.sampledIndices.reset(sampled);
    s.storageIndices.reset(storage);
    s.samplerIndices.reset(samplers);

    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sampled, VK_SHADER_STAGE_ALL, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, storage, VK_SHADER_STAGE_ALL, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_SAMPLER, samplers, VK_SHADER_STAGE_ALL, nullptr};
    VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    const VkDescriptorBindingFlags bindingFlags[3] = {flags, flags, flags};
    VkDescriptorSetLayoutBindingFlagsCreateInfo fi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    fi.bindingCount = 3;
    fi.pBindingFlags = bindingFlags;
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.pNext = &fi;
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    li.bindingCount = 3;
    li.pBindings = bindings;
    OX_VK_CHECK(vkCreateDescriptorSetLayout(s.device, &li, nullptr, &s.bindlessLayout));

    VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sampled},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, storage},
                                     {VK_DESCRIPTOR_TYPE_SAMPLER, samplers}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pi.maxSets = 1;
    pi.poolSizeCount = 3;
    pi.pPoolSizes = sizes;
    OX_VK_CHECK(vkCreateDescriptorPool(s.device, &pi, nullptr, &s.bindlessPool));

    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = s.bindlessPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &s.bindlessLayout;
    OX_VK_CHECK(vkAllocateDescriptorSets(s.device, &ai, &s.bindlessSet));

    VkPushConstantRange pc{VK_SHADER_STAGE_ALL, 0, std::min(kMaxPushConstantSize, c.maxPushConstantsSize)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &s.bindlessLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pc;
    OX_VK_CHECK(vkCreatePipelineLayout(s.device, &pl, nullptr, &s.pipelineLayout));
}

VkImageView createView(DeviceState& s, TextureRecord& t, const TextureViewDesc& vd, VkImageAspectFlags aspect) {
    const TextureDesc& d = t.desc;
    const u32 mipCount = vd.range.mipCount == ~0u ? d.mipLevels - vd.range.baseMip : vd.range.mipCount;
    const u32 layerCount = vd.range.layerCount == ~0u ? d.arrayLayers - vd.range.baseLayer : vd.range.layerCount;
    VkImageViewType type = vd.type;
    if (type == VK_IMAGE_VIEW_TYPE_MAX_ENUM) {
        type = defaultViewType(d.type, layerCount);
        if (d.type == TextureType::Cube && layerCount % 6 != 0) {
            type = layerCount > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        }
    }
    VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ci.image = t.image;
    ci.viewType = type;
    ci.format = vd.format == VK_FORMAT_UNDEFINED ? d.format : vd.format;
    ci.subresourceRange = {aspect, vd.range.baseMip, mipCount, vd.range.baseLayer, layerCount};
    VkImageView view = VK_NULL_HANDLE;
    OX_VK_CHECK(vkCreateImageView(s.device, &ci, nullptr, &view));
    return view;
}

void createDefaultViewsAndIndices(Device& device, DeviceState& s, TextureRecord& rec) {
    const FormatInfo fi = formatInfo(rec.desc.format);
    rec.aspect = formatAspect(rec.desc.format);
    // Sampling a depth/stencil image reads one aspect: default to depth.
    const VkImageAspectFlags sampleAspect = fi.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : rec.aspect;
    rec.defaultView = createView(s, rec, {}, sampleAspect);
    device.setDebugName(VK_OBJECT_TYPE_IMAGE, u64(rec.image), rec.desc.name);
    device.setDebugName(VK_OBJECT_TYPE_IMAGE_VIEW, u64(rec.defaultView), rec.desc.name);
    if (any(rec.desc.usage & TextureUsage::Sampled) && rec.desc.samples == 1) {
        rec.sampledIndex = s.sampledIndices.allocate();
        if (rec.sampledIndex == kInvalidBindlessIndex) {
            OX_LOG_ERROR("rhi", "bindless sampled image heap exhausted ({} entries)", s.sampledIndices.capacity());
        } else {
            s.writeBindlessSampled(rec.sampledIndex, rec.defaultView);
        }
    }
    rec.storageIndices.assign(rec.desc.mipLevels, kInvalidBindlessIndex);
}

TextureHandle createTextureAliased(Device& device, const TextureDesc& descIn, VmaAllocation memory) {
    DeviceState& s = device.state();
    TextureRecord rec;
    rec.desc = normalizedDesc(descIn);
    const VkImageCreateInfo ci = makeImageInfo(rec.desc);
    OX_VK_CHECK(vmaCreateAliasingImage(s.allocator, memory, &ci, &rec.image));
    createDefaultViewsAndIndices(device, s, rec);
    std::lock_guard lock(s.resourceMutex);
    return s.textures.allocate(std::move(rec));
}

} // namespace detail

// ---------------------------------------------------------------------------------------------------------------
// Buffers

namespace {

void destroyBufferRecord(DeviceState& s, BufferRecord& b) {
    if (b.allocation) {
        vmaDestroyBuffer(s.allocator, b.buffer, b.allocation);
    } else {
        vkDestroyBuffer(s.device, b.buffer, nullptr); // aliased placement: memory owned by the render graph
    }
}

void destroyTextureRecord(DeviceState& s, TextureRecord& t) {
    for (auto& v : t.views) vkDestroyImageView(s.device, v.view, nullptr);
    if (t.defaultView) vkDestroyImageView(s.device, t.defaultView, nullptr);
    if (!t.external) {
        if (t.allocation) {
            vmaDestroyImage(s.allocator, t.image, t.allocation);
        } else {
            vkDestroyImage(s.device, t.image, nullptr);
        }
    }
    s.sampledIndices.free(t.sampledIndex);
    for (u32 i : t.storageIndices) s.storageIndices.free(i);
}

} // namespace

namespace detail {
BufferHandle createBufferAliased(Device& device, const BufferDesc& desc, VmaAllocation memory) {
    DeviceState& s = device.state();
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = desc.size;
    bi.usage = toVkBufferUsage(desc.usage);
    BufferRecord rec;
    rec.desc = desc;
    OX_VK_CHECK(vmaCreateAliasingBuffer(s.allocator, memory, &bi, &rec.buffer));
    VkBufferDeviceAddressInfo addr{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    addr.buffer = rec.buffer;
    rec.address = vkGetBufferDeviceAddress(s.device, &addr);
    device.setDebugName(VK_OBJECT_TYPE_BUFFER, u64(rec.buffer), desc.name);
    std::lock_guard lock(s.resourceMutex);
    return s.buffers.allocate(std::move(rec));
}
} // namespace detail

BufferHandle Device::createBuffer(const BufferDesc& desc, const void* initialData) {
    DeviceState& s = *m_s;
    OX_ASSERT(desc.size > 0, "buffer '{}' has zero size", desc.name);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = desc.size;
    bi.usage = toVkBufferUsage(desc.usage);
    if (s.caps.accelerationStructure) {
        if (any(desc.usage & BufferUsage::AccelStructStorage)) bi.usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR;
        if (any(desc.usage & BufferUsage::AccelStructInput)) bi.usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    }
    if (s.caps.rayTracingPipeline && any(desc.usage & BufferUsage::ShaderBindingTable)) {
        bi.usage |= VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR;
    }
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo ai{};
    switch (desc.memory) {
    case MemoryUsage::GpuOnly: ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE; break;
    case MemoryUsage::Upload:
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    case MemoryUsage::Readback:
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    case MemoryUsage::Dynamic:
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    }
    BufferRecord rec;
    rec.desc = desc;
    VmaAllocationInfo info{};
    OX_VK_CHECK(vmaCreateBuffer(s.allocator, &bi, &ai, &rec.buffer, &rec.allocation, &info));
    rec.mapped = info.pMappedData;
    VkBufferDeviceAddressInfo addr{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    addr.buffer = rec.buffer;
    rec.address = vkGetBufferDeviceAddress(s.device, &addr);
    setDebugName(VK_OBJECT_TYPE_BUFFER, u64(rec.buffer), desc.name);
    if (!desc.name.empty()) vmaSetAllocationName(s.allocator, rec.allocation, desc.name.c_str());
    BufferHandle h;
    {
        std::lock_guard lock(s.resourceMutex);
        h = s.buffers.allocate(std::move(rec));
    }
    if (initialData) writeBuffer(h, initialData, desc.size);
    return h;
}

void Device::destroy(BufferHandle buffer) {
    DeviceState& s = *m_s;
    std::optional<BufferRecord> rec;
    {
        std::lock_guard lock(s.resourceMutex);
        rec = s.buffers.release(buffer);
    }
    if (!rec) return;
    s.retire([&s, r = std::move(*rec)]() mutable { destroyBufferRecord(s, r); });
}

const BufferDesc& Device::desc(BufferHandle b) const { return m_s->buffers.at(b).desc; }
VkBuffer Device::vkBuffer(BufferHandle b) const {
    const BufferRecord* r = m_s->buffers.get(b);
    return r ? r->buffer : VK_NULL_HANDLE;
}
VkDeviceAddress Device::address(BufferHandle b) const { return m_s->buffers.at(b).address; }
void* Device::mapped(BufferHandle b) const { return m_s->buffers.at(b).mapped; }
bool Device::isAlive(BufferHandle b) const { return m_s->buffers.get(b) != nullptr; }

void Device::writeBuffer(BufferHandle buffer, const void* data, u64 size, u64 offset) {
    DeviceState& s = *m_s;
    BufferRecord& rec = s.buffers.at(buffer);
    OX_ASSERT(offset + size <= rec.desc.size, "writeBuffer out of range on '{}'", rec.desc.name);
    if (rec.mapped) {
        std::memcpy(static_cast<u8*>(rec.mapped) + offset, data, size);
        vmaFlushAllocation(s.allocator, rec.allocation, offset, size);
        return;
    }
    BufferHandle staging = createBuffer({size, BufferUsage::TransferSrc, MemoryUsage::Upload, "staging.write"});
    BufferRecord& st = s.buffers.at(staging);
    std::memcpy(st.mapped, data, size);
    vmaFlushAllocation(s.allocator, st.allocation, 0, size);
    immediateSubmit([&](CommandList& cmd) {
        cmd.copyBuffer(staging, buffer, size, 0, offset);
        cmd.memoryBarrier(Access::TransferWrite, Access::General);
    });
    std::optional<BufferRecord> r = s.buffers.release(staging);
    destroyBufferRecord(s, *r);
}

std::vector<u8> Device::readBuffer(BufferHandle buffer, u64 offset, u64 size) {
    DeviceState& s = *m_s;
    BufferRecord& rec = s.buffers.at(buffer);
    if (size == VK_WHOLE_SIZE) size = rec.desc.size - offset;
    OX_ASSERT(offset + size <= rec.desc.size, "readBuffer out of range on '{}'", rec.desc.name);
    std::vector<u8> out(size);
    if (rec.mapped) {
        vmaInvalidateAllocation(s.allocator, rec.allocation, offset, size);
        std::memcpy(out.data(), static_cast<const u8*>(rec.mapped) + offset, size);
        return out;
    }
    BufferHandle staging = createBuffer({size, BufferUsage::TransferDst, MemoryUsage::Readback, "staging.read"});
    immediateSubmit([&](CommandList& cmd) {
        cmd.memoryBarrier(Access::General, Access::TransferRead);
        cmd.copyBuffer(buffer, staging, size, offset, 0);
        cmd.memoryBarrier(Access::TransferWrite, Access::HostRead);
    });
    BufferRecord& st = s.buffers.at(staging);
    vmaInvalidateAllocation(s.allocator, st.allocation, 0, size);
    std::memcpy(out.data(), st.mapped, size);
    std::optional<BufferRecord> r = s.buffers.release(staging);
    destroyBufferRecord(s, *r);
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// Textures

TextureHandle Device::createTexture(const TextureDesc& descIn) {
    DeviceState& s = *m_s;
    TextureRecord rec;
    rec.desc = normalizedDesc(descIn);
    const VkImageCreateInfo ci = makeImageInfo(rec.desc);
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (any(rec.desc.usage & (TextureUsage::ColorAttachment | TextureUsage::DepthStencilAttachment)) &&
        u64(rec.desc.width) * rec.desc.height >= 1024ull * 1024) {
        ai.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    }
    if (VkResult r = vmaCreateImage(s.allocator, &ci, &ai, &rec.image, &rec.allocation, nullptr); r != VK_SUCCESS) {
        OX_LOG_ERROR("rhi", "failed to create texture '{}' ({}x{} {}): {}", rec.desc.name, rec.desc.width, rec.desc.height,
                     formatName(rec.desc.format), vkResultName(r));
        return {};
    }
    if (!rec.desc.name.empty()) vmaSetAllocationName(s.allocator, rec.allocation, rec.desc.name.c_str());
    createDefaultViewsAndIndices(*this, s, rec);
    std::lock_guard lock(s.resourceMutex);
    return s.textures.allocate(std::move(rec));
}

TextureHandle Device::registerExternalTexture(VkImage image, const TextureDesc& desc, Access currentAccess) {
    DeviceState& s = *m_s;
    TextureRecord rec;
    rec.desc = normalizedDesc(desc);
    rec.image = image;
    rec.external = true;
    rec.tracked = currentAccess;
    createDefaultViewsAndIndices(*this, s, rec);
    std::lock_guard lock(s.resourceMutex);
    return s.textures.allocate(std::move(rec));
}

void Device::destroy(TextureHandle texture) {
    DeviceState& s = *m_s;
    std::optional<TextureRecord> rec;
    {
        std::lock_guard lock(s.resourceMutex);
        rec = s.textures.release(texture);
    }
    if (!rec) return;
    s.retire([&s, r = std::move(*rec)]() mutable { destroyTextureRecord(s, r); });
}

const TextureDesc& Device::desc(TextureHandle t) const { return m_s->textures.at(t).desc; }
VkImage Device::vkImage(TextureHandle t) const {
    const TextureRecord* r = m_s->textures.get(t);
    return r ? r->image : VK_NULL_HANDLE;
}
bool Device::isAlive(TextureHandle t) const { return m_s->textures.get(t) != nullptr; }
u32 Device::sampledIndex(TextureHandle t) const { return m_s->textures.at(t).sampledIndex; }
Access Device::trackedAccess(TextureHandle t) const { return m_s->textures.at(t).tracked; }
void Device::setTrackedAccess(TextureHandle t, Access a) {
    if (TextureRecord* r = m_s->textures.get(t)) r->tracked = a;
}

namespace detail {
VkImageView textureView(DeviceState& s, TextureRecord& rec, const TextureViewDesc& vd, VkImageAspectFlags aspect) {
    if (aspect == 0) aspect = formatInfo(rec.desc.format).depth ? VK_IMAGE_ASPECT_DEPTH_BIT : rec.aspect;
    for (const auto& v : rec.views) {
        if (v.desc == vd && v.aspect == aspect) return v.view;
    }
    VkImageView view = createView(s, rec, vd, aspect);
    rec.views.push_back({vd, aspect, view});
    return view;
}
} // namespace detail

VkImageView Device::view(TextureHandle texture, const TextureViewDesc& vd) {
    DeviceState& s = *m_s;
    std::lock_guard lock(s.resourceMutex);
    TextureRecord& rec = s.textures.at(texture);
    if (vd == TextureViewDesc{}) return rec.defaultView;
    return textureView(s, rec, vd, 0);
}

u32 Device::storageIndex(TextureHandle texture, u32 mip) {
    DeviceState& s = *m_s;
    std::lock_guard lock(s.resourceMutex);
    TextureRecord& rec = s.textures.at(texture);
    OX_ASSERT(any(rec.desc.usage & TextureUsage::Storage), "texture '{}' lacks TextureUsage::Storage", rec.desc.name);
    OX_ASSERT(mip < rec.desc.mipLevels);
    if (rec.storageIndices[mip] != kInvalidBindlessIndex) return rec.storageIndices[mip];
    TextureViewDesc vd;
    vd.range = {mip, 1, 0, ~0u};
    if (rec.desc.type == TextureType::Cube || rec.desc.arrayLayers > 1) {
        vd.type = rec.desc.type == TextureType::Tex1D ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    }
    VkImageView v = textureView(s, rec, vd, rec.aspect);
    const u32 idx = s.storageIndices.allocate();
    if (idx == kInvalidBindlessIndex) {
        OX_LOG_ERROR("rhi", "bindless storage image heap exhausted");
        return idx;
    }
    s.writeBindlessStorage(idx, v);
    rec.storageIndices[mip] = idx;
    return idx;
}

MemoryRequirements Device::memoryRequirements(const TextureDesc& descIn) {
    DeviceState& s = *m_s;
    const TextureDesc d = normalizedDesc(descIn);
    const VkImageCreateInfo ci = makeImageInfo(d);
    VkMemoryRequirements req{};
    VkImage tmp = VK_NULL_HANDLE;
    OX_VK_CHECK(vkCreateImage(s.device, &ci, nullptr, &tmp));
    vkGetImageMemoryRequirements(s.device, tmp, &req);
    vkDestroyImage(s.device, tmp, nullptr);
    return {req.size, req.alignment, req.memoryTypeBits};
}

MemoryRequirements Device::memoryRequirements(const BufferDesc& desc) {
    DeviceState& s = *m_s;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = desc.size;
    bi.usage = toVkBufferUsage(desc.usage);
    VkBuffer tmp = VK_NULL_HANDLE;
    OX_VK_CHECK(vkCreateBuffer(s.device, &bi, nullptr, &tmp));
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(s.device, tmp, &req);
    vkDestroyBuffer(s.device, tmp, nullptr);
    return {req.size, req.alignment, req.memoryTypeBits};
}

namespace {

struct UploadLayout {
    std::vector<VkBufferImageCopy> regions;
    u64 totalSize = 0;
};

UploadLayout computeUploadLayout(const TextureDesc& d, const TextureUploadDesc& u, u64 dataSize, u64 baseOffset) {
    UploadLayout out;
    const u32 layerCount = u.layerCount == ~0u ? d.arrayLayers - u.baseLayer : u.layerCount;
    const u32 maxMips = d.mipLevels - u.baseMip;
    const u32 mipCount = u.mipCount == ~0u ? maxMips : std::min(u.mipCount, maxMips);
    const FormatInfo fi = formatInfo(d.format);
    u64 offset = 0;
    for (u32 m = 0; m < mipCount; ++m) {
        const u32 mip = u.baseMip + m;
        const u64 levelSize = mipLevelSize(d.format, d.width, d.height, d.depth, mip) * layerCount;
        if (u.mipCount == ~0u && offset + levelSize > dataSize) break; // data holds fewer mips
        VkBufferImageCopy r{};
        r.bufferOffset = baseOffset + offset;
        r.imageSubresource = {fi.depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : formatAspect(d.format), mip,
                              u.baseLayer, layerCount};
        r.imageExtent = {std::max(1u, d.width >> mip), std::max(1u, d.height >> mip), std::max(1u, d.depth >> mip)};
        out.regions.push_back(r);
        offset += levelSize;
    }
    out.totalSize = offset;
    return out;
}

} // namespace

void Device::uploadTexture(TextureHandle texture, std::span<const u8> data, const TextureUploadDesc& u) {
    DeviceState& s = *m_s;
    const TextureDesc d = s.textures.at(texture).desc;
    const UploadLayout layout = computeUploadLayout(d, u, data.size(), 0);
    OX_ASSERT(!layout.regions.empty() && layout.totalSize <= data.size(), "uploadTexture: '{}' needs {} bytes, got {}",
              d.name, layout.totalSize, data.size());
    BufferHandle staging = createBuffer({layout.totalSize, BufferUsage::TransferSrc, MemoryUsage::Upload, "staging.texture"});
    BufferRecord& st = s.buffers.at(staging);
    std::memcpy(st.mapped, data.data(), layout.totalSize);
    vmaFlushAllocation(s.allocator, st.allocation, 0, layout.totalSize);
    const VkBuffer stagingBuffer = st.buffer;
    immediateSubmit([&](CommandList& cmd) {
        cmd.transition(texture, Access::TransferWrite, true);
        vkCmdCopyBufferToImage(cmd.vk(), stagingBuffer, s.textures.at(texture).image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               u32(layout.regions.size()), layout.regions.data());
        cmd.transition(texture, u.finalAccess);
    });
    std::optional<BufferRecord> r = s.buffers.release(staging);
    destroyBufferRecord(s, *r);
}

std::vector<u8> Device::readTexture(TextureHandle texture, u32 mip, u32 layer) {
    DeviceState& s = *m_s;
    const TextureDesc d = s.textures.at(texture).desc;
    OX_ASSERT(d.samples == 1, "readTexture on a multisampled texture; resolve first");
    const u64 size = mipLevelSize(d.format, d.width, d.height, d.depth, mip);
    BufferHandle staging = createBuffer({size, BufferUsage::TransferDst, MemoryUsage::Readback, "staging.readback"});
    const Access before = s.textures.at(texture).tracked;
    immediateSubmit([&](CommandList& cmd) {
        cmd.transition(texture, Access::TransferRead);
        cmd.copyTextureToBuffer(texture, mip, layer, staging, 0);
        cmd.memoryBarrier(Access::TransferWrite, Access::HostRead);
        if (before != Access::Undefined && before != Access::TransferRead) cmd.transition(texture, before);
    });
    BufferRecord& st = s.buffers.at(staging);
    vmaInvalidateAllocation(s.allocator, st.allocation, 0, size);
    std::vector<u8> out(size);
    std::memcpy(out.data(), st.mapped, size);
    std::optional<BufferRecord> r = s.buffers.release(staging);
    destroyBufferRecord(s, *r);
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// Samplers

SamplerHandle Device::sampler(const SamplerDesc& desc) {
    DeviceState& s = *m_s;
    std::lock_guard lock(s.resourceMutex);
    for (const auto& [d, h] : s.samplerCache) {
        if (d == desc) return h;
    }
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = desc.magFilter;
    ci.minFilter = desc.minFilter;
    ci.mipmapMode = desc.mipmapMode;
    ci.addressModeU = desc.addressU;
    ci.addressModeV = desc.addressV;
    ci.addressModeW = desc.addressW;
    ci.mipLodBias = desc.mipLodBias;
    ci.anisotropyEnable = desc.maxAnisotropy > 1.f && s.caps.samplerAnisotropy;
    ci.maxAnisotropy = ci.anisotropyEnable ? std::min(desc.maxAnisotropy, s.caps.maxSamplerAnisotropy) : 1.f;
    ci.compareEnable = desc.compareEnable;
    ci.compareOp = desc.compareOp;
    ci.minLod = desc.minLod;
    ci.maxLod = desc.maxLod;
    ci.borderColor = desc.borderColor;
    SamplerRecord rec;
    rec.desc = desc;
    OX_VK_CHECK(vkCreateSampler(s.device, &ci, nullptr, &rec.sampler));
    rec.index = s.samplerIndices.allocate();
    OX_ASSERT(rec.index != kInvalidBindlessIndex, "bindless sampler heap exhausted");
    s.writeBindlessSampler(rec.index, rec.sampler);
    SamplerHandle h = s.samplers.allocate(rec);
    s.samplerCache.emplace_back(desc, h);
    return h;
}

SamplerHandle Device::defaultSampler(DefaultSampler d) const { return m_s->defaultSamplers[u32(d)]; }
u32 Device::samplerIndex(SamplerHandle h) const { return m_s->samplers.at(h).index; }
VkSampler Device::vkSampler(SamplerHandle h) const { return m_s->samplers.at(h).sampler; }

namespace detail {

void createDefaultSamplers(Device& device, DeviceState& s) {
    SamplerDesc linearRepeat;
    SamplerDesc linearClamp;
    linearClamp.addressU = linearClamp.addressV = linearClamp.addressW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    SamplerDesc nearestRepeat;
    nearestRepeat.magFilter = nearestRepeat.minFilter = VK_FILTER_NEAREST;
    nearestRepeat.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    SamplerDesc nearestClamp = nearestRepeat;
    nearestClamp.addressU = nearestClamp.addressV = nearestClamp.addressW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    SamplerDesc aniso;
    aniso.maxAnisotropy = 16.f;
    SamplerDesc shadow = linearClamp;
    shadow.addressU = shadow.addressV = shadow.addressW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    shadow.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    shadow.compareEnable = true;
    shadow.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    const SamplerDesc all[] = {linearRepeat, linearClamp, nearestRepeat, nearestClamp, aniso, shadow};
    for (u32 i = 0; i < u32(DefaultSampler::Count); ++i) {
        s.defaultSamplers[i] = device.sampler(all[i]);
        OX_ASSERT(s.samplers.at(s.defaultSamplers[i]).index == i, "default sampler indices must be stable");
    }
}

void destroyAllResources(Device& device, DeviceState& s) {
    (void)device;
    u32 leaked = 0;
    s.pipelines.forEach([&](PipelineHandle, PipelineRecord& p) {
        vkDestroyPipeline(s.device, p.pipeline, nullptr);
        if (p.sbt) ++leaked;
    });
    s.accelStructs.forEach([&](AccelStructHandle, AccelStructRecord& a) {
        vkDestroyAccelerationStructureKHR(s.device, a.handle, nullptr);
    });
    s.textures.forEach([&](TextureHandle, TextureRecord& t) {
        if (!t.external) ++leaked;
        destroyTextureRecord(s, t);
    });
    s.buffers.forEach([&](BufferHandle, BufferRecord& b) { destroyBufferRecord(s, b); });
    s.samplers.forEach([&](SamplerHandle, SamplerRecord& r) { vkDestroySampler(s.device, r.sampler, nullptr); });
    const usize live = s.buffers.size() + s.textures.size();
    if (live > 2) {
        OX_LOG_DEBUG("rhi", "destroying {} buffers / {} textures still alive at device shutdown", s.buffers.size(),
                     s.textures.size());
    }
    (void)leaked;
}

// -------------------------------------------------------------------------------------------------------------
// Staging ring (async uploads)

StagingAllocation DeviceState::allocateStaging(u64 size, u64 alignment) {
    StagingAllocation out;
    BufferRecord& ring = buffers.at(stagingRing);
    auto alignUp = [](u64 v, u64 a) { return (v + a - 1) / a * a; };
    if (size > ringSize / 2) {
        return out; // caller falls back to a dedicated staging buffer
    }
    for (;;) {
        // Reclaim completed regions.
        while (!ringFences.empty()) {
            const auto& [end, point] = ringFences.front();
            u64 v = 0;
            vkGetSemaphoreCounterValue(device, queues[queueOf[u32(point.queue)]].timeline, &v);
            if (point.value == 0 || v < point.value) break;
            ringTail = end;
            ringFences.pop_front();
        }
        const bool empty = ringFences.empty() && ringOpen.empty();
        if (empty) {
            ringHead = ringTail = 0;
        }
        u64 candidate = alignUp(ringHead, alignment);
        bool ok = false;
        if (empty) {
            candidate = 0;
            ok = true;
        } else if (ringHead > ringTail) {
            if (candidate + size <= ringSize) {
                ok = true;
            } else if (size < ringTail) {
                candidate = 0;
                ok = true;
            }
        } else if (ringHead < ringTail) {
            ok = candidate + size < ringTail;
        }
        if (ok) {
            out.offset = candidate;
            ringHead = candidate + size;
            ringOpen.push_back(ringHead);
            out.buffer = ring.buffer;
            out.handle = stagingRing;
            out.ptr = static_cast<u8*>(ring.mapped) + candidate;
            return out;
        }
        // Full: make sure the oldest region has a fence, then wait for it.
        if (ringFences.empty()) {
            return out; // everything open in the current batch; caller flushes and retries
        }
        const TimelinePoint p = ringFences.front().second;
        VkSemaphore sem = queues[queueOf[u32(p.queue)]].timeline;
        VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wi.semaphoreCount = 1;
        wi.pSemaphores = &sem;
        wi.pValues = &p.value;
        vkWaitSemaphores(device, &wi, ~0ull);
    }
}

} // namespace detail

// ---------------------------------------------------------------------------------------------------------------
// Async uploads

namespace {

CommandList& uploadCommandList(Device& device, DeviceState& s) {
    if (!s.uploadCmd || s.uploadFrame != s.frameNumber) {
        s.uploadCmd = &device.commandList(QueueType::Transfer, "uploads");
        s.uploadFrame = s.frameNumber;
    }
    return *s.uploadCmd;
}

// Returns (buffer, offset) of staging memory holding `data`; dedicated buffers are retired after the flush.
std::pair<VkBuffer, u64> stage(Device& device, DeviceState& s, std::span<const u8> data) {
    StagingAllocation a = s.allocateStaging(data.size(), 16);
    if (!a.buffer && data.size() <= s.ringSize / 2) {
        device.flushUploads();
        a = s.allocateStaging(data.size(), 16);
    }
    if (a.buffer) {
        std::memcpy(a.ptr, data.data(), data.size());
        vmaFlushAllocation(s.allocator, s.buffers.at(s.stagingRing).allocation, a.offset, data.size());
        return {a.buffer, a.offset};
    }
    BufferHandle h = device.createBuffer({data.size(), BufferUsage::TransferSrc, MemoryUsage::Upload, "staging.large"});
    std::memcpy(device.mapped(h), data.data(), data.size());
    vmaFlushAllocation(s.allocator, s.buffers.at(h).allocation, 0, data.size());
    const VkBuffer vb = device.vkBuffer(h);
    device.destroy(h); // deferred: retires after the upload submission completes
    return {vb, 0};
}

} // namespace

void Device::uploadBufferAsync(BufferHandle dst, std::span<const u8> data, u64 dstOffset, Access consumerAccess) {
    DeviceState& s = *m_s;
    const BufferRecord& rec = s.buffers.at(dst);
    OX_ASSERT(dstOffset + data.size() <= rec.desc.size, "uploadBufferAsync out of range on '{}'", rec.desc.name);
    if (rec.mapped) {
        writeBuffer(dst, data.data(), data.size(), dstOffset);
        return;
    }
    auto [src, srcOffset] = stage(*this, s, data);
    CommandList& cmd = uploadCommandList(*this, s);
    VkBufferCopy region{srcOffset, dstOffset, data.size()};
    vkCmdCopyBuffer(cmd.vk(), src, rec.buffer, 1, &region);
    const u32 tf = s.queue(QueueType::Transfer).family;
    const u32 gf = s.queue(QueueType::Graphics).family;
    VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.buffer = rec.buffer;
    b.offset = dstOffset;
    b.size = data.size();
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    if (tf != gf) {
        b.srcQueueFamilyIndex = tf;
        b.dstQueueFamilyIndex = gf;
        b.offset = 0;
        b.size = VK_WHOLE_SIZE;
        s.uploadAcquires.push_back({false, dst, {}, consumerAccess, VK_IMAGE_LAYOUT_UNDEFINED});
    } else {
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    }
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.bufferMemoryBarrierCount = 1;
    di.pBufferMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd.vk(), &di);
}

void Device::uploadTextureAsync(TextureHandle dst, std::span<const u8> data, const TextureUploadDesc& u) {
    DeviceState& s = *m_s;
    TextureRecord& rec = s.textures.at(dst);
    const UploadLayout layoutProbe = computeUploadLayout(rec.desc, u, data.size(), 0);
    OX_ASSERT(!layoutProbe.regions.empty() && layoutProbe.totalSize <= data.size(), "uploadTextureAsync: not enough data");
    auto [src, srcOffset] = stage(*this, s, data.subspan(0, layoutProbe.totalSize));
    const UploadLayout layout = computeUploadLayout(rec.desc, u, data.size(), srcOffset);
    CommandList& cmd = uploadCommandList(*this, s);
    const u32 tf = s.queue(QueueType::Transfer).family;
    const u32 gf = s.queue(QueueType::Graphics).family;
    const AccessInfo fin = accessInfo(u.finalAccess);

    VkImageMemoryBarrier2 toDst = makeImageBarrier(s, rec, 0, 0, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                                   VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, QueueType::Transfer);
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &toDst;
    vkCmdPipelineBarrier2(cmd.vk(), &di);
    vkCmdCopyBufferToImage(cmd.vk(), src, rec.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, u32(layout.regions.size()),
                           layout.regions.data());
    VkImageMemoryBarrier2 rel = makeImageBarrier(s, rec, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                                 0, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, fin.layout, QueueType::Transfer);
    if (tf != gf) {
        rel.srcQueueFamilyIndex = tf;
        rel.dstQueueFamilyIndex = gf;
        s.uploadAcquires.push_back({true, {}, dst, u.finalAccess, rel.newLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL});
    } else {
        rel.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        rel.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
    }
    di.pImageMemoryBarriers = &rel;
    vkCmdPipelineBarrier2(cmd.vk(), &di);
    rec.tracked = u.finalAccess;
}

TimelinePoint Device::flushUploads() {
    DeviceState& s = *m_s;
    if (!s.uploadCmd) {
        return s.pendingUploadWait.value_or(TimelinePoint{QueueType::Transfer, 0});
    }
    CommandList& cmd = *s.uploadCmd;
    s.uploadCmd = nullptr;
    OX_VK_CHECK(vkEndCommandBuffer(cmd.vk()));
    const VkCommandBuffer cbs[] = {cmd.vk()};
    const u64 v = submitRaw(s, QueueType::Transfer, cbs, {}, {});
    const TimelinePoint p{QueueType::Transfer, v};
    for (u64 end : s.ringOpen) s.ringFences.emplace_back(end, p);
    s.ringOpen.clear();
    s.pendingAcquires.insert(s.pendingAcquires.end(), s.uploadAcquires.begin(), s.uploadAcquires.end());
    s.uploadAcquires.clear();
    s.pendingUploadWait = p;
    return p;
}

} // namespace ox::rhi
