#pragma once

#include <oxwald/rhi/device.hpp>
#include <oxwald/rhi/swapchain.hpp>

#include "file_poller.hpp"

#include <vk_mem_alloc.h>

#include <array>
#include <chrono>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace ox::rhi::detail {

struct BufferRecord {
    BufferDesc desc;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    void* mapped = nullptr;
    VkDeviceAddress address = 0;
};

struct TextureRecord {
    TextureDesc desc;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr; // null for external images and aliased placements
    bool external = false;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageView defaultView = VK_NULL_HANDLE;
    struct View {
        TextureViewDesc desc;
        VkImageAspectFlags aspect = 0;
        VkImageView view = VK_NULL_HANDLE;
    };
    std::vector<View> views;
    u32 sampledIndex = kInvalidBindlessIndex;
    std::vector<u32> storageIndices; // per mip
    Access tracked = Access::Undefined;
};

struct SamplerRecord {
    SamplerDesc desc;
    VkSampler sampler = VK_NULL_HANDLE;
    u32 index = kInvalidBindlessIndex;
};

struct PipelineRecord {
    PipelineKind kind = PipelineKind::Graphics;
    std::string name;
    GraphicsPipelineDesc graphics;
    ComputePipelineDesc compute;
    RayTracingPipelineDesc rayTracing;
    VkPipeline pipeline = VK_NULL_HANDLE;
    std::vector<std::filesystem::path> dependencies;
    u32 version = 0;
    std::array<u32, 3> localSize{1, 1, 1};
    // Ray tracing shader binding table.
    BufferHandle sbt;
    VkStridedDeviceAddressRegionKHR regions[4]{}; // raygen, miss, hit, callable
};

struct AccelStructRecord {
    bool tlas = false;
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    BufferHandle buffer;
    u64 address = 0;
    BlasDesc blas;
    TlasDesc tlasDesc;
    BufferHandle scratch;
    u64 scratchSize = 0;
    BufferHandle instances; // TLAS: framesInFlight × maxInstances VkAccelerationStructureInstanceKHR
    u32 builtInstances = 0;
};

struct PhysicalQueue {
    VkQueue queue = VK_NULL_HANDLE;
    u32 family = 0;
    u32 index = 0;
    VkSemaphore timeline = VK_NULL_HANDLE;
    u64 submitted = 0;
    bool timestamps = false;
    std::unique_ptr<std::mutex> mutex = std::make_unique<std::mutex>();
};

struct TimestampScope {
    std::string name;
    u32 depth = 0;
    u32 beginQuery = 0;
    u32 endQuery = ~0u;
};

struct FrameContext {
    std::array<VkCommandPool, kQueueTypeCount> pools{};
    std::array<std::vector<VkCommandBuffer>, kQueueTypeCount> buffers;
    std::array<u32, kQueueTypeCount> used{};
    std::deque<CommandList> lists;
    std::array<u64, kQueueTypeCount> waitValues{}; // per physical queue
    u64 frameNumber = 0;
    VkQueryPool queryPool = VK_NULL_HANDLE;
    u32 queryCount = 0;
    std::vector<TimestampScope> scopes;
};

struct DeferredDeletion {
    i64 frame = -1;                             // retire once this frame completed on the GPU (-1 = none)
    std::array<u64, kQueueTypeCount> values{};  // and these timeline values (out-of-frame submits)
    std::function<void()> destroy;
};

struct PendingAcquire {
    bool texture = false;
    BufferHandle buffer;
    TextureHandle tex;
    Access dstAccess = Access::General;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;    // new layout (must match the release barrier)
    VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; // old layout of the release barrier
};

struct StagingAllocation {
    VkBuffer buffer = VK_NULL_HANDLE;
    BufferHandle handle;
    u64 offset = 0;
    u8* ptr = nullptr;
};

struct DeviceState {
    DeviceDesc desc;
    DeviceCaps caps;
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    ISurfaceProvider* surfaceProvider = nullptr;
    VmaAllocator allocator = nullptr;
    std::vector<PhysicalQueue> queues;           // unique VkQueues
    std::array<u32, kQueueTypeCount> queueOf{}; // QueueType -> physical queue index
    VkPhysicalDeviceFeatures2 enabledFeatures{};
    VkPipelineStageFlags2 supportedStages = ~0ull;
    VkAccessFlags2 supportedAccess = ~0ull;

    // Resources
    std::recursive_mutex resourceMutex;
    HandlePool<BufferRecord, BufferTag> buffers;
    HandlePool<TextureRecord, TextureTag> textures;
    HandlePool<SamplerRecord, SamplerTag> samplers;
    HandlePool<PipelineRecord, PipelineTag> pipelines;
    HandlePool<AccelStructRecord, AccelStructTag> accelStructs;
    std::vector<std::pair<SamplerDesc, SamplerHandle>> samplerCache;
    std::array<SamplerHandle, u32(DefaultSampler::Count)> defaultSamplers{};

    // Bindless
    VkDescriptorSetLayout bindlessLayout = VK_NULL_HANDLE;
    VkDescriptorPool bindlessPool = VK_NULL_HANDLE;
    VkDescriptorSet bindlessSet = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    IndexAllocator sampledIndices;
    IndexAllocator storageIndices;
    IndexAllocator samplerIndices;

    // Pipelines & shaders
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    std::unique_ptr<ShaderCompiler> compiler;
    FilePoller shaderFiles;
    std::chrono::steady_clock::time_point lastShaderPoll{};
    std::string lastPipelineError;

    // Frames
    std::vector<FrameContext> frames;
    u64 frameNumber = 0;
    i64 completedFrame = -1;
    bool inFrame = false;
    std::vector<GpuTiming> lastTimings;
    std::mutex deletionMutex;
    std::deque<DeferredDeletion> deletions;
    std::array<VkCommandPool, kQueueTypeCount> immediatePools{};
    std::mutex immediateMutex;

    // Uploads
    BufferHandle stagingRing;
    u64 ringSize = 0;
    u64 ringHead = 0;
    u64 ringTail = 0;
    std::deque<std::pair<u64, TimelinePoint>> ringFences; // (end offset, point)
    std::vector<u64> ringOpen;                              // allocations waiting for the next upload flush
    CommandList* uploadCmd = nullptr;
    u64 uploadFrame = ~0ull;
    std::vector<PendingAcquire> uploadAcquires;   // recorded, not yet flushed
    std::vector<PendingAcquire> pendingAcquires;  // flushed, waiting for the next graphics submit
    std::optional<TimelinePoint> pendingUploadWait;

    // Profiling
    void* tracyCtx = nullptr;
    RenderDocCapture renderDoc;
    bool debugUtils = false;

    // helpers (implemented in device.cpp / resources.cpp)
    PhysicalQueue& queue(QueueType q) { return queues[queueOf[u32(q)]]; }
    void retire(std::function<void()> fn);
    void collectGarbage(bool all);
    VkPipelineStageFlags2 sanitizeStages(VkPipelineStageFlags2 s, QueueType q) const;
    VkAccessFlags2 sanitizeAccess(VkAccessFlags2 a) const;
    StagingAllocation allocateStaging(u64 size, u64 alignment);
    void writeBindlessSampled(u32 index, VkImageView view);
    void writeBindlessStorage(u32 index, VkImageView view);
    void writeBindlessSampler(u32 index, VkSampler sampler);
};

// Shared helpers
const char* vkResultName(VkResult r);
VkImageSubresourceRange fullRange(const TextureRecord& t, const TextureSubresource& r = {});
VkImageMemoryBarrier2 makeImageBarrier(const DeviceState& s, const TextureRecord& t, VkPipelineStageFlags2 srcStages,
                                       VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStages, VkAccessFlags2 dstAccess,
                                       VkImageLayout oldLayout, VkImageLayout newLayout, QueueType queue,
                                       const TextureSubresource& range = {});
VkBufferUsageFlags toVkBufferUsage(BufferUsage u);
VkImageUsageFlags toVkImageUsage(TextureUsage u, VkFormat format);
VkImageCreateInfo makeImageInfo(const TextureDesc& desc);
TextureDesc normalizedDesc(TextureDesc desc);
void createDefaultViewsAndIndices(Device& device, DeviceState& s, TextureRecord& rec);
TextureHandle createTextureAliased(Device& device, const TextureDesc& desc, VmaAllocation memory);
VkImageView textureView(DeviceState& s, TextureRecord& rec, const TextureViewDesc& vd, VkImageAspectFlags aspect);

} // namespace ox::rhi::detail

#define OX_VK_CHECK(expr)                                                                                       \
    do {                                                                                                        \
        const VkResult _r = (expr);                                                                             \
        OX_ASSERT(_r == VK_SUCCESS, "{} failed: {}", #expr, ::ox::rhi::detail::vkResultName(_r));               \
    } while (false)
