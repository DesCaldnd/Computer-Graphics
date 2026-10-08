#pragma once

#include <oxwald/rhi/access.hpp>
#include <oxwald/rhi/acceleration_structure.hpp>
#include <oxwald/rhi/types.hpp>

#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ox::rhi {

class Device;

struct ColorAttachment {
    TextureHandle texture;
    u32 mip = 0;
    u32 layer = 0;
    VkAttachmentLoadOp load = VK_ATTACHMENT_LOAD_OP_CLEAR;
    VkAttachmentStoreOp store = VK_ATTACHMENT_STORE_OP_STORE;
    ClearColor clear{};
    TextureHandle resolve; // MSAA resolve target (average)
};

struct DepthAttachment {
    TextureHandle texture;
    u32 mip = 0;
    u32 layer = 0;
    VkAttachmentLoadOp load = VK_ATTACHMENT_LOAD_OP_CLEAR;
    VkAttachmentStoreOp store = VK_ATTACHMENT_STORE_OP_STORE;
    ClearDepthStencil clear{};
    bool readOnly = false;
};

struct RenderingDesc {
    std::vector<ColorAttachment> colors;
    std::optional<DepthAttachment> depth;
    VkRect2D area{}; // zero extent = full size of the first attachment's mip
    u32 layerCount = 1;
    u32 viewMask = 0; // multiview
    bool setViewportAndScissor = true;
    // The scope is filled only by executeSecondary() (VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT); viewport
    // and scissor are then set by the secondaries (Device::secondaryCommandList).
    bool secondaryContents = false;
};

// Attachment formats / extent a secondary command list inherits (multithreaded recording inside one rendering scope).
struct SecondaryRenderingInfo {
    std::vector<VkFormat> colorFormats;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkFormat stencilFormat = VK_FORMAT_UNDEFINED;
    u32 viewMask = 0;
    u32 samples = 1;
    VkRect2D area{}; // viewport + scissor set at begin
};

// Thin wrapper over a VkCommandBuffer. Obtained from Device::commandList() (per-frame, recycled automatically)
// or inside Device::immediateSubmit(). Texture barriers through transition() keep the device-side layout tracking
// in sync; the render graph emits barriers itself and updates the tracking at the end.
class CommandList {
public:
    CommandList(Device& device, VkCommandBuffer cmd, QueueType queue) : m_device(&device), m_cmd(cmd), m_queue(queue) {}

    [[nodiscard]] VkCommandBuffer vk() const { return m_cmd; }
    [[nodiscard]] QueueType queue() const { return m_queue; }
    [[nodiscard]] Device& device() const { return *m_device; }

    // --- synchronization (synchronization2) ---
    void transition(TextureHandle texture, Access newAccess, bool discardContents = false);
    void textureBarrier(TextureHandle texture, Access src, Access dst, TextureSubresource range = {}, bool discard = false);
    void bufferBarrier(BufferHandle buffer, Access src, Access dst, u64 offset = 0, u64 size = VK_WHOLE_SIZE);
    void memoryBarrier(Access src, Access dst);
    void pipelineBarrier(const VkDependencyInfo& info);

    // --- rendering ---
    void beginRendering(const RenderingDesc& desc);
    void endRendering();
    void setViewport(f32 x, f32 y, f32 width, f32 height, f32 minDepth = 0.f, f32 maxDepth = 1.f);
    void setScissor(i32 x, i32 y, u32 width, u32 height);
    void setDepthBias(f32 constant, f32 clamp, f32 slope);
    void setStencilReference(u32 reference);

    void bindPipeline(PipelineHandle pipeline);
    void pushConstants(const void* data, u32 size, u32 offset = 0);
    template <class T>
    void pushConstants(const T& value) {
        static_assert(sizeof(T) <= 128, "push constants are limited to 128 bytes");
        pushConstants(&value, sizeof(T));
    }
    void bindVertexBuffer(u32 binding, BufferHandle buffer, u64 offset = 0);
    void bindIndexBuffer(BufferHandle buffer, u64 offset = 0, VkIndexType type = VK_INDEX_TYPE_UINT32);

    void draw(u32 vertexCount, u32 instanceCount = 1, u32 firstVertex = 0, u32 firstInstance = 0);
    void drawIndexed(u32 indexCount, u32 instanceCount = 1, u32 firstIndex = 0, i32 vertexOffset = 0, u32 firstInstance = 0);
    void drawIndirect(BufferHandle args, u64 offset, u32 drawCount, u32 stride = sizeof(VkDrawIndirectCommand));
    void drawIndexedIndirect(BufferHandle args, u64 offset, u32 drawCount, u32 stride = sizeof(VkDrawIndexedIndirectCommand));
    void drawIndirectCount(BufferHandle args, u64 offset, BufferHandle count, u64 countOffset, u32 maxDraws,
                           u32 stride = sizeof(VkDrawIndirectCommand));
    void drawIndexedIndirectCount(BufferHandle args, u64 offset, BufferHandle count, u64 countOffset, u32 maxDraws,
                                  u32 stride = sizeof(VkDrawIndexedIndirectCommand));
    void drawMeshTasks(u32 x, u32 y = 1, u32 z = 1);
    // VkDrawMeshTasksIndirectCommandEXT (3 × u32) records.
    void drawMeshTasksIndirect(BufferHandle args, u64 offset, u32 drawCount = 1, u32 stride = 12);

    // --- compute / ray tracing ---
    void dispatch(u32 x, u32 y = 1, u32 z = 1);
    // Group count from the bound compute pipeline's local size.
    void dispatchThreads(u32 x, u32 y = 1, u32 z = 1);
    void dispatchIndirect(BufferHandle args, u64 offset = 0);
    void traceRays(u32 width, u32 height, u32 depth = 1); // uses the bound RT pipeline's shader binding table

    // --- transfer ---
    void copyBuffer(BufferHandle src, BufferHandle dst, u64 size, u64 srcOffset = 0, u64 dstOffset = 0);
    void copyBufferToTexture(BufferHandle src, u64 srcOffset, TextureHandle dst, u32 mip = 0, u32 layer = 0, u32 layerCount = 1);
    void copyTextureToBuffer(TextureHandle src, u32 mip, u32 layer, BufferHandle dst, u64 dstOffset = 0, u32 layerCount = 1);
    void copyTexture(TextureHandle src, TextureHandle dst, u32 mip = 0, u32 layer = 0);
    void blitTexture(TextureHandle src, u32 srcMip, u32 srcLayer, TextureHandle dst, u32 dstMip, u32 dstLayer,
                     VkFilter filter = VK_FILTER_LINEAR);
    // Expect the texture in TransferWrite (use transition()).
    void clearTexture(TextureHandle texture, const ClearColor& color);
    void clearDepthStencil(TextureHandle texture, const ClearDepthStencil& value);
    void fillBuffer(BufferHandle buffer, u32 value, u64 offset = 0, u64 size = VK_WHOLE_SIZE);
    void updateBuffer(BufferHandle buffer, const void* data, u64 size, u64 offset = 0); // ≤ 64 KiB
    // Blits mip 0 down the chain (all layers); leaves the texture in `finalAccess`.
    void generateMipmaps(TextureHandle texture, Access finalAccess = Access::SampledGraphics);

    // --- acceleration structures (require DeviceCaps::accelerationStructure) ---
    void buildTlas(AccelStructHandle tlas, std::span<const TlasInstance> instances, bool update = false);
    void refitBlas(AccelStructHandle blas); // re-builds in update mode from the original geometry buffers
    // Update-mode rebuild from new geometry (same triangle counts/topology, e.g. ping-pong skinning output buffers).
    void refitBlas(AccelStructHandle blas, const BlasDesc& geometry);

    // --- debugging & profiling ---
    // --- secondary command lists (multithreaded recording) ---
    // Records `lists` (ended secondaries from Device::secondaryCommandList) into this primary, in order.
    void executeSecondary(std::span<CommandList* const> lists);
    // Ends a secondary command list (primaries are ended by Device::submit).
    void end();
    [[nodiscard]] bool isSecondary() const { return m_secondary; }

    void beginLabel(std::string_view name, const f32 color[4] = nullptr);
    void endLabel();
    void insertLabel(std::string_view name);
    void beginTimestamp(std::string_view name);
    void endTimestamp();
    // Manual Tracy GPU zone (shows up on the "Graphics queue" GPU track next to the render graph passes). Only
    // recorded on the graphics queue while a Tracy profiler is connected (OX_ENABLE_TRACY builds); otherwise a cheap
    // no-op. Zones nest and must be closed in the same command list.
    void beginGpuZone(std::string_view name);
    void endGpuZone();
    [[nodiscard]] u32 openGpuZones() const { return u32(m_gpuZones.size()); }

    struct ScopedGpuZone {
        CommandList& cmd;
        ScopedGpuZone(CommandList& c, std::string_view name) : cmd(c) { cmd.beginGpuZone(name); }
        ~ScopedGpuZone() { cmd.endGpuZone(); }
        ScopedGpuZone(const ScopedGpuZone&) = delete;
        ScopedGpuZone& operator=(const ScopedGpuZone&) = delete;
    };

    struct ScopedLabel {
        CommandList& cmd;
        ScopedLabel(CommandList& c, std::string_view name) : cmd(c) { cmd.beginLabel(name); }
        ~ScopedLabel() { cmd.endLabel(); }
    };

private:
    friend class Device;
    Device* m_device;
    VkCommandBuffer m_cmd;
    QueueType m_queue;
    PipelineHandle m_boundPipeline;
    i32 m_frameSlot = -1; // frame context owning this list (timestamps); -1 for immediate submits
    bool m_secondary = false;
    std::vector<u32> m_openTimestamps;
    std::vector<void*> m_gpuZones; // Tracy zone objects (nullptr when not recording)
};

// OX_RHI_GPU_ZONE(cmd, "Bloom") — scoped manual Tracy GPU zone on a CommandList.
#define OX_RHI_GPU_ZONE_CAT2(a, b) a##b
#define OX_RHI_GPU_ZONE_CAT(a, b) OX_RHI_GPU_ZONE_CAT2(a, b)
#define OX_RHI_GPU_ZONE(cmd, name) \
    ::ox::rhi::CommandList::ScopedGpuZone OX_RHI_GPU_ZONE_CAT(oxGpuZone_, __LINE__)((cmd), (name))

} // namespace ox::rhi
