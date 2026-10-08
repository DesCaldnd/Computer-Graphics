#pragma once

#include <oxwald/rhi/access.hpp>
#include <oxwald/rhi/acceleration_structure.hpp>
#include <oxwald/rhi/command_list.hpp>
#include <oxwald/rhi/device_caps.hpp>
#include <oxwald/rhi/pipeline.hpp>
#include <oxwald/rhi/profiling.hpp>
#include <oxwald/rhi/shader_compiler.hpp>
#include <oxwald/rhi/types.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ox {
class JobSystem;
}

namespace ox::rhi {

class ISurfaceProvider;
class Swapchain;

namespace detail {
struct DeviceState;
}

struct DeviceDesc {
    std::string appName = "OxwaldEngine";
    // nullopt = on in Debug/RelWithDebInfo builds, off in Release; env OX_VULKAN_VALIDATION=0/1 overrides.
    std::optional<bool> validation;
    ISurfaceProvider* surface = nullptr; // nullptr = headless (tests, tools, offline baking)
    u32 framesInFlight = 2;              // 2..3
    bool enableRayTracing = true;        // enable RT extensions when present
    bool enableMeshShaders = true;
    bool preferDiscreteGpu = true;
    bool asyncCompute = true;  // request a separate compute queue
    bool asyncTransfer = true; // request a separate transfer queue
    std::filesystem::path pipelineCachePath; // empty = in-memory only
    ShaderCompilerOptions shaderOptions = ShaderCompilerOptions::defaults();
    bool shaderHotReload = true;
    u32 hotReloadPollMs = 200;
    u64 stagingRingSize = 32ull << 20;
    u32 maxBindlessSampledImages = 16384; // clamped to DeviceCaps
    u32 maxBindlessStorageImages = 4096;
    u32 maxBindlessSamplers = 128;
    // Extra extensions enabled when available (silently skipped otherwise), e.g. what NVIDIA NGX/DLSS needs
    // (render::appendUpscalerVulkanExtensions fills these).
    std::vector<std::string> optionalInstanceExtensions;
    std::vector<std::string> optionalDeviceExtensions;
};

struct SemaphoreWait {
    VkSemaphore semaphore = VK_NULL_HANDLE;
    u64 value = 0; // 0 for binary semaphores
    VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
};

struct SubmitInfo {
    std::vector<TimelinePoint> waits;           // cross-queue dependencies (e.g. async compute results)
    std::vector<SemaphoreWait> waitSemaphores;   // external semaphores
    std::vector<VkSemaphore> signalSemaphores;   // external binary semaphores
    Swapchain* swapchain = nullptr;              // wait for acquire, signal the present semaphore
};

struct TextureUploadDesc {
    u32 baseMip = 0;
    u32 mipCount = ~0u;   // ~0u = every mip present in the data
    u32 baseLayer = 0;
    u32 layerCount = ~0u; // ~0u = all layers (cube faces: +X,-X,+Y,-Y,+Z,-Z)
    Access finalAccess = Access::SampledGraphics;
};

// Vulkan device + everything that lives exactly once per device: queues, VMA, bindless heap, pipeline cache,
// shader compiler/hot reload, frame contexts, deferred destruction, staging ring. volk keeps global function
// pointers, so only one Device may exist at a time.
class Device {
public:
    static std::unique_ptr<Device> create(const DeviceDesc& desc = {}, std::string* error = nullptr);
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    [[nodiscard]] const DeviceCaps& caps() const;
    [[nodiscard]] VkInstance vkInstance() const;
    [[nodiscard]] VkPhysicalDevice vkPhysicalDevice() const;
    [[nodiscard]] VkDevice vkDevice() const;
    [[nodiscard]] VkQueue vkQueue(QueueType q) const;
    [[nodiscard]] u32 queueFamily(QueueType q) const;
    [[nodiscard]] VkSurfaceKHR surface() const;
    [[nodiscard]] ISurfaceProvider* surfaceProvider() const;
    [[nodiscard]] VkPipelineLayout pipelineLayout() const;
    [[nodiscard]] VkDescriptorSet bindlessSet() const;

    // --- buffers ---
    BufferHandle createBuffer(const BufferDesc& desc, const void* initialData = nullptr);
    void destroy(BufferHandle buffer);
    [[nodiscard]] const BufferDesc& desc(BufferHandle buffer) const;
    [[nodiscard]] VkBuffer vkBuffer(BufferHandle buffer) const;
    [[nodiscard]] VkDeviceAddress address(BufferHandle buffer) const;
    [[nodiscard]] void* mapped(BufferHandle buffer) const; // nullptr for GpuOnly
    [[nodiscard]] bool isAlive(BufferHandle buffer) const;
    // Mapped buffers: memcpy + flush. GPU-only buffers: staging copy + immediate submit.
    void writeBuffer(BufferHandle buffer, const void* data, u64 size, u64 offset = 0);
    // Synchronous readback (staging + immediate submit for GPU-only memory).
    std::vector<u8> readBuffer(BufferHandle buffer, u64 offset = 0, u64 size = VK_WHOLE_SIZE);

    // --- textures ---
    TextureHandle createTexture(const TextureDesc& desc);
    // Wraps an image owned elsewhere (swapchain, interop). Only views are destroyed with the handle.
    TextureHandle registerExternalTexture(VkImage image, const TextureDesc& desc, Access currentAccess = Access::Undefined);
    void destroy(TextureHandle texture);
    [[nodiscard]] const TextureDesc& desc(TextureHandle texture) const;
    [[nodiscard]] VkImage vkImage(TextureHandle texture) const;
    VkImageView view(TextureHandle texture, const TextureViewDesc& view = {});
    [[nodiscard]] bool isAlive(TextureHandle texture) const;
    // Bindless indices (engine/shaders/common/bindless.glsl). Sampled index exists for TextureUsage::Sampled.
    [[nodiscard]] u32 sampledIndex(TextureHandle texture) const;
    u32 storageIndex(TextureHandle texture, u32 mip = 0); // allocated on first use
    // Layout/access tracking used by CommandList::transition and the render graph.
    [[nodiscard]] Access trackedAccess(TextureHandle texture) const;
    void setTrackedAccess(TextureHandle texture, Access access);
    // Synchronous upload: data = for each mip, for each layer, tightly packed texels/blocks.
    void uploadTexture(TextureHandle texture, std::span<const u8> data, const TextureUploadDesc& desc = {});
    std::vector<u8> readTexture(TextureHandle texture, u32 mip = 0, u32 layer = 0);
    [[nodiscard]] MemoryRequirements memoryRequirements(const TextureDesc& desc);
    [[nodiscard]] MemoryRequirements memoryRequirements(const BufferDesc& desc);

    // --- samplers (cached by description; never destroyed before the device) ---
    SamplerHandle sampler(const SamplerDesc& desc);
    [[nodiscard]] SamplerHandle defaultSampler(DefaultSampler s) const;
    [[nodiscard]] u32 samplerIndex(SamplerHandle sampler) const;
    [[nodiscard]] VkSampler vkSampler(SamplerHandle sampler) const;

    // --- pipelines (handles stay valid across shader hot reloads) ---
    PipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc& desc);
    PipelineHandle createComputePipeline(const ComputePipelineDesc& desc);
    PipelineHandle createRayTracingPipeline(const RayTracingPipelineDesc& desc);
    // Background compilation (shaderc + vkCreate*Pipelines with the pipeline cache) on `jobs`: the handle is valid
    // immediately but has no VkPipeline (draws/dispatches with it are skipped; callers use a placeholder while
    // !isPipelineReady) and becomes ready at a later beginFrame() — never while command lists are being recorded.
    PipelineHandle createGraphicsPipelineAsync(const GraphicsPipelineDesc& desc, JobSystem& jobs);
    PipelineHandle createComputePipelineAsync(const ComputePipelineDesc& desc, JobSystem& jobs);
    [[nodiscard]] bool isPipelineReady(PipelineHandle pipeline) const;
    [[nodiscard]] u32 pendingPipelineCompiles() const;
    void destroy(PipelineHandle pipeline);
    [[nodiscard]] bool isAlive(PipelineHandle pipeline) const;
    [[nodiscard]] VkPipeline vkPipeline(PipelineHandle pipeline) const;
    [[nodiscard]] PipelineKind pipelineKind(PipelineHandle pipeline) const;
    [[nodiscard]] u32 pipelineVersion(PipelineHandle pipeline) const; // increments on every successful rebuild
    [[nodiscard]] std::string lastPipelineError() const;
    ShaderCompiler& shaderCompiler();
    // Polls shader files (and their includes); recompiles + swaps affected pipelines. Called from beginFrame().
    // force = skip the poll interval. Returns the number of rebuilt pipelines.
    u32 reloadChangedShaders(bool force = false);
    bool savePipelineCache();

    // --- frames & submission ---
    // Waits until the frame that used this frame slot retired, recycles its command lists, runs deferred
    // destruction, collects GPU timestamps and swaps hot-reloaded pipelines.
    void beginFrame();
    void endFrame();
    [[nodiscard]] u64 frameNumber() const;
    [[nodiscard]] u32 frameIndex() const;
    [[nodiscard]] u32 framesInFlight() const;
    // Command list valid until this frame slot comes around again.
    CommandList& commandList(QueueType queue = QueueType::Graphics, std::string_view debugName = {});
    TimelinePoint submit(CommandList& cmd, const SubmitInfo& info = {});
    // Secondary command list for multithreaded recording into a rendering scope begun with
    // RenderingDesc::secondaryContents (or a render graph pass declared with PassBuilder::secondaryCommandLists()).
    // `thread` selects a per-thread command pool (< kMaxRecordingThreads, e.g. JobSystem::currentThreadIndex()):
    // calls with different slots may run concurrently, one thread per slot at a time. Valid until this frame slot
    // comes around again; end() it, then CommandList::executeSecondary() on the primary.
    static constexpr u32 kMaxRecordingThreads = 64;
    CommandList& secondaryCommandList(u32 thread, const SecondaryRenderingInfo& info, QueueType queue = QueueType::Graphics);
    // Records and submits a one-off command list, then blocks until it finished. Works outside frames.
    void immediateSubmit(const std::function<void(CommandList&)>& record, QueueType queue = QueueType::Graphics);
    void wait(TimelinePoint point);
    [[nodiscard]] bool isComplete(TimelinePoint point) const;
    [[nodiscard]] TimelinePoint lastSubmitted(QueueType queue) const;
    [[nodiscard]] VkSemaphore timelineSemaphore(QueueType queue) const;
    void waitIdle();

    // --- asynchronous uploads (transfer queue + queue family ownership transfer) ---
    // The copy is recorded into a transfer command list that is submitted by flushUploads() (called by
    // beginFrame/endFrame). The next graphics/compute submit automatically waits for it and acquires ownership.
    void uploadBufferAsync(BufferHandle dst, std::span<const u8> data, u64 dstOffset = 0,
                           Access consumerAccess = Access::General);
    void uploadTextureAsync(TextureHandle dst, std::span<const u8> data, const TextureUploadDesc& desc = {});
    TimelinePoint flushUploads();

    // --- acceleration structures (DeviceCaps::accelerationStructure) ---
    AccelStructHandle createBlas(const BlasDesc& desc); // builds immediately (+ compaction)
    AccelStructHandle createTlas(const TlasDesc& desc); // build with CommandList::buildTlas
    void destroy(AccelStructHandle as);
    [[nodiscard]] u64 accelStructAddress(AccelStructHandle as) const;
    [[nodiscard]] VkAccelerationStructureKHR vkAccelStruct(AccelStructHandle as) const;

    // --- debugging & profiling ---
    void setDebugName(VkObjectType type, u64 handle, std::string_view name);
    [[nodiscard]] GpuMemoryStats memoryStats() const;
    [[nodiscard]] const std::vector<GpuTiming>& gpuTimings() const; // last retired frame
    [[nodiscard]] void* tracyGpuContext() const;                    // TracyVkCtx or nullptr
    RenderDocCapture& renderDoc();

    static u32 validationErrorCount();
    static u32 validationWarningCount();
    static void resetValidationCounters();

    detail::DeviceState& state() { return *m_s; }
    const detail::DeviceState& state() const { return *m_s; }

private:
    Device();
    bool init(const DeviceDesc& desc, std::string& error);
    std::unique_ptr<detail::DeviceState> m_s;
};

} // namespace ox::rhi
