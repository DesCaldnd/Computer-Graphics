#pragma once

#include <oxwald/rhi/render_graph.hpp>

#include <optional>

namespace ox::rhi {

struct RGAccessDecl {
    u32 resource = 0;
    bool texture = true;
    Access access = Access::Undefined;
    bool preserve = true; // depends on previous contents
};

struct RGAttachmentDecl {
    u32 resource = ~0u;
    VkAttachmentLoadOp load = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ClearColor clear{};
    ClearDepthStencil clearDepth{};
    u32 mip = 0;
    u32 layer = 0;
    bool readOnly = false;
    u32 resolveTarget = ~0u;
};

struct RGPassDecl {
    std::string name;
    PassType type = PassType::Graphics;
    QueueType queueHint = QueueType::Graphics;
    bool sideEffect = false;
    std::vector<RGAccessDecl> accesses;
    std::vector<RGAttachmentDecl> colors;
    std::optional<RGAttachmentDecl> depth;
    std::function<void(PassContext&)> fn;
};

struct RGResourceDecl {
    std::string name;
    bool texture = true;
    TextureDesc textureDesc;
    BufferDesc bufferDesc;
    bool imported = false;
    TextureHandle importedTexture;
    BufferHandle importedBuffer;
    RGImport import;
    bool output = false;
};

struct RenderGraph::Impl {
    std::vector<RGPassDecl> passes;
    std::vector<RGResourceDecl> resources;

    RenderGraphPlan plan;
    bool planValid = false;
    u32 compileCount = 0;

    // Physical resources for the cached plan (filled by execute()).
    u64 physicalHash = 0;
    Device* physicalDevice = nullptr;
    std::vector<TextureHandle> physicalTextures; // per resource id
    std::vector<BufferHandle> physicalBuffers;
    std::vector<void*> slotMemory; // VmaAllocation per alias slot (nullptr = dedicated resource)
    std::vector<TimelinePoint> lastExecution;

    u64 computeHash(const RGCompileOptions& options) const;
    void compileFull(const RGCompileOptions& options);
    TextureHandle textureHandle(u32 id) const;
    BufferHandle bufferHandle(u32 id) const;
    void recordPass(CommandList& cmd, Device& device, const RGPlannedPass& planned, bool timestamps);
};

} // namespace ox::rhi
