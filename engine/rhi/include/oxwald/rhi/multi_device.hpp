#pragma once

// Explicit multi-adapter support for split rendering: a secondary VkDevice (typically the integrated GPU) next to
// the primary rhi::Device, compute-only, running *portable* compute passes (bindless shaders whose push constants
// hold only bindless indices and plain values) with the same SPIR-V, plus cross-device channels that move textures
// between the devices (shared external memory, imported host memory, or staging + CPU copies) and a worker thread
// that synchronises the two timelines on the CPU.
//
// Vulkan has no cross-vendor device groups: an NVIDIA dGPU and an Intel iGPU are two unrelated VkDevices. Opaque
// memory/semaphore handles are only valid between devices with the same deviceUUID/driverUUID, so the paths that
// work between two *different* GPUs are VK_EXT_external_memory_host and staging copies. See
// docs/guide/35-split-rendering.md and docs/dev/modules/rhi.md ("Multi-device").
//
// The secondary device calls Vulkan through its own VolkDeviceTable (volk's global device functions belong to the
// primary). Only the primary rhi::Device may exist once per process; any number of SecondaryDevices may exist.

#include <oxwald/rhi/device.hpp>
#include <oxwald/rhi/split_scheduler.hpp>

#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ox::rhi {

struct AdapterInfo {
    u32 index = 0; // position in vkEnumeratePhysicalDevices of the primary's instance
    std::string name;
    VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    u32 vendorId = 0;
    u32 deviceId = 0;
    u32 apiVersion = 0;
    std::array<u8, VK_UUID_SIZE> deviceUuid{};
    std::array<u8, VK_UUID_SIZE> driverUuid{};
    std::array<u8, VK_LUID_SIZE> luid{};
    bool luidValid = false;
    u64 deviceLocalBytes = 0;
    bool primary = false;           // the primary Device's physical device
    bool suitable = false;          // can host a secondary device (Vulkan 1.2 + bindless + timeline + compute)
    std::string unsuitableReason;
    bool externalMemoryHost = false;   // VK_EXT_external_memory_host
    bool externalMemoryOpaque = false; // VK_KHR_external_memory_win32 (Windows) / _fd (Linux)
};
const char* adapterTypeName(VkPhysicalDeviceType type);
std::vector<AdapterInfo> enumerateAdapters(const Device& primary);
std::string describeAdapters(const std::vector<AdapterInfo>& adapters); // multi-line, for logs and reports

// Adds the optional primary-device extensions the shared-memory transfer paths need (external memory win32/fd,
// external memory host). Call before Device::create; without them split rendering uses the staging path.
void appendMultiDeviceExtensions(DeviceDesc& desc);

struct SecondaryDeviceDesc {
    i32 adapterIndex = -1;          // AdapterInfo::index; -1 = best adapter other than the primary (integrated first)
    bool allowSameAdapter = false;  // allow a second VkDevice on the primary's physical device (tests)
    CrossDeviceTransfer transfer = CrossDeviceTransfer::Auto;
    bool calibrate = true;          // measure the secondary/primary speed ratio with a post-process-like kernel
    u32 maxTextures = 256;          // bindless capacity of the secondary heap
};

// One compute dispatch of a portable pass, recorded on the secondary device. `pipeline` is the primary device's
// compute pipeline: the secondary recompiles the same desc (cached per pipelineVersion, so hot reload works).
struct PortableDispatch {
    struct Binding {
        u32 pushOffset = 0; // byte offset of the u32 bindless index in `push`
        u32 texture = 0;    // index into CrossDeviceChannelDesc::textures
        bool storage = false;
    };
    std::string name;
    PipelineHandle pipeline;
    std::array<u8, kMaxPushConstantSize> push{};
    u32 pushSize = 0;
    std::vector<Binding> bindings;
    std::array<u32, 3> threads{1, 1, 1}; // dispatchThreads semantics (rounded up by the pipeline's local size)
};

struct CrossDeviceChannelDesc {
    std::string name;
    std::vector<TextureDesc> textures; // 2D, one mip, one layer, single sample
    std::vector<u32> inputs;           // textures uploaded from the primary every job
    std::vector<u32> outputs;          // textures downloaded to the primary every job
    u32 slots = 3;                     // jobs in flight (latency + 1)
};

struct CrossDeviceJobTiming {
    f64 uploadMs = 0.0;   // secondary: copy shared/staging buffer -> input textures
    f64 computeMs = 0.0;  // secondary: the dispatches
    f64 downloadMs = 0.0; // secondary: copy outputs -> shared/staging buffer
    f64 cpuCopyMs = 0.0;  // worker: staging memcpys (Staging path)
    f64 latencyMs = 0.0;  // CPU: primary export retired -> results available
};

class SecondaryDevice;

// Moves a fixed set of textures primary -> secondary, runs portable dispatches there, and moves results back.
// Slots are reused round-robin; a slot's job must be waited for before the slot is exported to again.
class CrossDeviceChannel {
public:
    ~CrossDeviceChannel(); // waits for this channel's jobs; primary objects are destroyed after the GPU used them
    CrossDeviceChannel(const CrossDeviceChannel&) = delete;
    CrossDeviceChannel& operator=(const CrossDeviceChannel&) = delete;

    [[nodiscard]] const CrossDeviceChannelDesc& desc() const;
    [[nodiscard]] CrossDeviceTransfer path() const;
    [[nodiscard]] u64 uploadBytes() const;   // per job
    [[nodiscard]] u64 downloadBytes() const; // per job

    // Primary side, inside a render graph pass on the graphics queue. Inputs must be in Access::TransferRead
    // (TRANSFER_SRC_OPTIMAL), outputs in Access::TransferWrite (TRANSFER_DST_OPTIMAL); one handle per desc entry.
    void recordExport(CommandList& cmd, u32 slot, std::span<const TextureHandle> inputs);
    void recordImport(CommandList& cmd, u32 slot, std::span<const TextureHandle> outputs);

    // Queues the secondary work of `slot` once the primary submission that recorded its export retired
    // (`exportDone`, a point on the primary's graphics timeline). `ticket` identifies the job (e.g. frame number).
    bool submit(u32 slot, u64 ticket, TimelinePoint exportDone, std::vector<PortableDispatch> dispatches);
    // Blocks until the job `ticket` of `slot` finished. false: not submitted, failed (device lost) or timed out.
    bool wait(u32 slot, u64 ticket, u32 timeoutMs = 5000);
    [[nodiscard]] bool ready(u32 slot, u64 ticket) const;
    [[nodiscard]] CrossDeviceJobTiming timing(u32 slot) const;

    struct Impl;
    explicit CrossDeviceChannel(std::unique_ptr<Impl> impl);

private:
    std::unique_ptr<Impl> m_impl;
};

class SecondaryDevice {
public:
    static std::unique_ptr<SecondaryDevice> create(Device& primary, const SecondaryDeviceDesc& desc = {},
                                                   std::string* error = nullptr);
    ~SecondaryDevice(); // channels must be destroyed first
    SecondaryDevice(const SecondaryDevice&) = delete;
    SecondaryDevice& operator=(const SecondaryDevice&) = delete;

    [[nodiscard]] const AdapterInfo& adapter() const;
    [[nodiscard]] const CrossDeviceCaps& caps() const;
    [[nodiscard]] CrossDeviceTransfer transferPath() const;
    [[nodiscard]] const std::string& transferReason() const;
    [[nodiscard]] bool sameAdapter() const;
    // Secondary GPU time / primary GPU time of the calibration kernel (0 = not measured).
    [[nodiscard]] f32 calibratedSpeedRatio() const;
    [[nodiscard]] f64 calibrationPrimaryMs() const;
    [[nodiscard]] f64 calibrationSecondaryMs() const;
    [[nodiscard]] VkDevice vkDevice() const;
    [[nodiscard]] Device& primary() const;

    // Device lost (or a simulated loss): every pending and future job fails; the owner should fall back.
    [[nodiscard]] bool lost() const;
    [[nodiscard]] std::string lostReason() const;
    void simulateDeviceLost(); // tests: the next job fails as if the device returned VK_ERROR_DEVICE_LOST

    // nullptr + error when a format/usage is unsupported on either device or memory cannot be shared.
    std::unique_ptr<CrossDeviceChannel> createChannel(const CrossDeviceChannelDesc& desc, std::string* error = nullptr);
    // Compiles (or finds) the secondary version of a primary compute pipeline. false + error when it cannot run there.
    bool preparePipeline(PipelineHandle primaryPipeline, std::string* error = nullptr);
    void waitIdle();

    // Live SecondaryDevice objects in the process (leak tests).
    static u32 liveCount();

    struct Impl;

private:
    SecondaryDevice();
    std::unique_ptr<Impl> m_impl;
    friend class CrossDeviceChannel;
};

} // namespace ox::rhi
