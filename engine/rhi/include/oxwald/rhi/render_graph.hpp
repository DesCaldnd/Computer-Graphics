#pragma once

#include <oxwald/rhi/access.hpp>
#include <oxwald/rhi/command_list.hpp>
#include <oxwald/rhi/types.hpp>

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ox::rhi {

class Device;
class Swapchain;

struct RGTexture {
    u32 id = ~0u;
    [[nodiscard]] bool valid() const { return id != ~0u; }
    bool operator==(const RGTexture&) const = default;
};
struct RGBuffer {
    u32 id = ~0u;
    [[nodiscard]] bool valid() const { return id != ~0u; }
    bool operator==(const RGBuffer&) const = default;
};

enum class PassType : u8 { Graphics, Compute, Transfer, RayTracing };
const char* passTypeName(PassType t);

struct RGImport {
    Access initial = Access::Undefined; // state the resource is in when the graph starts (Undefined = discard)
    Access final = Access::Undefined;   // state to leave it in (Undefined = whatever the last pass used)
};

class RenderGraph;
class PassContext;

// Declares how a pass uses resources. One declaration per resource per pass.
class PassBuilder {
public:
    // Reads previous contents.
    PassBuilder& read(RGTexture t, Access access);
    PassBuilder& read(RGBuffer b, Access access);
    // Read-modify-write: depends on the previous writer (e.g. storage image accumulation, partial copies).
    PassBuilder& write(RGTexture t, Access access);
    PassBuilder& write(RGBuffer b, Access access);
    // Full overwrite: previous contents are irrelevant (e.g. a compute pass writing every texel).
    PassBuilder& overwrite(RGTexture t, Access access);
    PassBuilder& overwrite(RGBuffer b, Access access);
    // Render target for an automatically begun dynamic rendering scope. LOAD = read-modify-write, CLEAR/DONT_CARE = overwrite.
    PassBuilder& color(RGTexture t, VkAttachmentLoadOp load = VK_ATTACHMENT_LOAD_OP_CLEAR, ClearColor clear = {},
                       u32 mip = 0, u32 layer = 0);
    PassBuilder& depth(RGTexture t, VkAttachmentLoadOp load = VK_ATTACHMENT_LOAD_OP_CLEAR, ClearDepthStencil clear = {},
                       bool readOnly = false);
    PassBuilder& resolve(RGTexture msaaColor, RGTexture target); // resolve `msaaColor` attachment into `target`
    // Queue hint: Compute = async compute if the device has a separate queue (and the graph enables it).
    PassBuilder& queue(QueueType hint);
    // Never culled (e.g. readback, debug capture, writes to memory the graph does not know about).
    PassBuilder& sideEffect();
    PassBuilder& execute(std::function<void(PassContext&)> fn);

private:
    friend class RenderGraph;
    PassBuilder(RenderGraph& g, u32 pass) : m_graph(&g), m_pass(pass) {}
    RenderGraph* m_graph;
    u32 m_pass;
};

// --- compiled plan (pure CPU data, inspectable in tests) ---

enum class RGBarrierKind : u8 { Normal, Aliasing, Release, Acquire, Final };

struct RGBarrier {
    u32 resource = 0;
    bool texture = true;
    RGBarrierKind kind = RGBarrierKind::Normal;
    Access srcAccess = Access::Undefined; // previous use (for dumps/tests)
    Access dstAccess = Access::Undefined;
    VkPipelineStageFlags2 srcStages = 0;
    VkAccessFlags2 srcAccessMask = 0;
    VkPipelineStageFlags2 dstStages = 0;
    VkAccessFlags2 dstAccessMask = 0;
    VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    u32 srcFamily = VK_QUEUE_FAMILY_IGNORED;
    u32 dstFamily = VK_QUEUE_FAMILY_IGNORED;
};

struct RGPlannedPass {
    u32 pass = 0; // index into the declared passes
    QueueType queue = QueueType::Graphics;
    u32 batch = 0;
    std::vector<RGBarrier> before;
    std::vector<RGBarrier> after; // queue ownership releases, final transitions of imported resources
};

struct RGBatch {
    QueueType queue = QueueType::Graphics;
    std::vector<u32> passes;      // indices into RenderGraphPlan::passes
    std::vector<u32> waitBatches; // batches on other queues that must signal first
};

struct RGResourcePlan {
    bool texture = true;
    bool imported = false;
    bool used = false;      // accessed by at least one surviving pass
    u32 firstPass = ~0u;    // indices into RenderGraphPlan::passes
    u32 lastPass = 0;
    i32 aliasSlot = -1;     // transient resources: memory slot
    bool asyncQueue = false; // touched by a non-graphics queue (never aliased)
    u64 size = 0;
    u64 alignment = 1;
    u32 memoryTypeBits = ~0u;
    TextureUsage derivedTextureUsage = TextureUsage::None;
    BufferUsage derivedBufferUsage = BufferUsage::None;
    Access finalAccess = Access::Undefined; // state after the graph ran
};

struct RGAliasSlot {
    bool texture = true;
    u64 size = 0;
    u64 alignment = 1;
    u32 memoryTypeBits = ~0u;
    std::vector<u32> resources; // ordered by lifetime
};

struct RenderGraphPlan {
    std::vector<RGPlannedPass> passes; // execution order
    std::vector<u32> culledPasses;
    std::vector<RGBatch> batches;
    std::vector<RGResourcePlan> resources;
    std::vector<RGAliasSlot> aliasSlots;
    std::vector<std::string> warnings;
    u64 hash = 0;
    u64 transientBytesUnaliased = 0;
    u64 transientBytesAliased = 0;

    [[nodiscard]] u32 barrierCount() const;
    [[nodiscard]] const RGPlannedPass* findPass(u32 declaredPass) const;
    [[nodiscard]] std::string dump(const RenderGraph& graph) const;
};

struct RGCompileOptions {
    bool cull = true;
    bool aliasing = true;
    bool asyncCompute = false;  // honour PassBuilder::queue(Compute)
    bool asyncTransfer = false; // honour PassBuilder::queue(Transfer)
    std::array<u32, 3> queueFamilies{0, 0, 0}; // per QueueType; equal families = no ownership transfers
    // Real memory requirements; defaults to estimateTextureSize()/buffer size (CPU tests).
    std::function<MemoryRequirements(const TextureDesc&)> textureRequirements;
    std::function<MemoryRequirements(const BufferDesc&)> bufferRequirements;
};

struct RGExecuteOptions {
    Swapchain* swapchain = nullptr; // first graphics batch waits for acquire, last one signals present
    std::vector<TimelinePoint> waits;
    bool timestamps = true;
    bool asyncCompute = true; // use the device's async compute queue for passes hinted Compute
};

// Frame graph. Typical frame:  graph.reset();  declare resources/passes;  graph.execute(device, opts);
// compile() is cached by a hash of the declared topology, physical transient resources by plan hash.
class RenderGraph {
public:
    RenderGraph();
    ~RenderGraph();
    RenderGraph(const RenderGraph&) = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;

    void reset(); // forget declarations; keeps the cached plan and physical resources

    RGTexture createTexture(const TextureDesc& desc);
    RGBuffer createBuffer(const BufferDesc& desc);
    RGTexture importTexture(TextureHandle texture, const TextureDesc& desc, RGImport import);
    // Uses the device's tracked state as the initial access.
    RGTexture importTexture(Device& device, TextureHandle texture, Access finalAccess = Access::Undefined);
    RGBuffer importBuffer(BufferHandle buffer, const BufferDesc& desc, RGImport import = {Access::General, Access::Undefined});
    void markOutput(RGTexture t);
    void markOutput(RGBuffer b);

    PassBuilder addPass(std::string name, PassType type = PassType::Graphics);

    const RenderGraphPlan& compile(const RGCompileOptions& options = {});
    [[nodiscard]] const RenderGraphPlan& plan() const;
    [[nodiscard]] u32 compileCount() const; // number of non-cached compiles (tests)

    // Multi-queue execution: creates command lists, submits batches with timeline waits.
    void execute(Device& device, const RGExecuteOptions& options = {});
    // Records every pass into `cmd` (single queue; async hints ignored). Caller submits.
    void execute(CommandList& cmd);
    void releaseResources(Device& device);

    [[nodiscard]] std::string exportGraphviz() const;

    // Introspection
    [[nodiscard]] u32 passCount() const;
    [[nodiscard]] const std::string& passName(u32 pass) const;
    [[nodiscard]] u32 resourceCount() const;
    [[nodiscard]] const std::string& resourceName(u32 resource) const;
    [[nodiscard]] TextureHandle physicalTexture(RGTexture t) const; // valid after execute()
    [[nodiscard]] BufferHandle physicalBuffer(RGBuffer b) const;

    struct Impl;

private:
    friend class PassBuilder;
    friend class PassContext;
    std::unique_ptr<Impl> m_impl;
};

class PassContext {
public:
    CommandList& cmd;
    Device& device;

    [[nodiscard]] TextureHandle texture(RGTexture t) const;
    [[nodiscard]] BufferHandle buffer(RGBuffer b) const;
    [[nodiscard]] u32 sampledIndex(RGTexture t) const;
    [[nodiscard]] u32 storageIndex(RGTexture t, u32 mip = 0) const;
    [[nodiscard]] VkDeviceAddress address(RGBuffer b) const;
    [[nodiscard]] VkExtent2D extent(RGTexture t, u32 mip = 0) const;

    PassContext(CommandList& c, Device& d, const RenderGraph::Impl& g) : cmd(c), device(d), m_graph(g) {}

private:
    const RenderGraph::Impl& m_graph;
};

} // namespace ox::rhi
