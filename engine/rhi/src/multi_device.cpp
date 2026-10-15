// Secondary VkDevice + cross-device channels (see multi_device.hpp).
//
// Threading: the owner (render thread) creates channels, resolves pipelines and queues jobs; one worker thread per
// SecondaryDevice waits for the primary's timeline, does the staging memcpys, records + submits the secondary command
// buffer and waits for it. The secondary queue, command pool and query pool are only touched by the worker (and by
// create() before the worker starts).
#include "device_impl.hpp"

#include <oxwald/core/assert.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/rhi/format.hpp>
#include <oxwald/rhi/multi_device.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <mutex>
#include <thread>
#include <unordered_map>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <malloc.h>
#include <vulkan/vulkan_win32.h>
#else
#include <unistd.h>
#endif

namespace ox::rhi {

using namespace detail;
using Clock = std::chrono::steady_clock;

namespace {

std::atomic<u32> g_liveSecondary{0};

constexpr const char* kExtMemHost = "VK_EXT_external_memory_host";
constexpr const char* kExtSync2 = "VK_KHR_synchronization2";
#ifdef _WIN32
constexpr const char* kExtMemOpaque = "VK_KHR_external_memory_win32";
constexpr VkExternalMemoryHandleTypeFlagBits kOpaqueHandle = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
constexpr const char* kExtMemOpaque = "VK_KHR_external_memory_fd";
constexpr VkExternalMemoryHandleTypeFlagBits kOpaqueHandle = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif
constexpr VkExternalMemoryHandleTypeFlagBits kHostHandle = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
constexpr VkBufferUsageFlags kTransferBufferUsage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
constexpr u64 kRegionAlignment = 256;
constexpr u32 kQueriesPerJob = 4;

f64 msSince(Clock::time_point t) { return std::chrono::duration<f64, std::milli>(Clock::now() - t).count(); }
u64 alignUp(u64 v, u64 a) { return a ? (v + a - 1) / a * a : v; }

std::vector<VkExtensionProperties> deviceExtensionList(VkPhysicalDevice pd) {
    u32 n = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> e(n);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, e.data());
    return e;
}

bool hasExt(const std::vector<VkExtensionProperties>& exts, const char* name) {
    return std::any_of(exts.begin(), exts.end(), [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
}

u32 findMemoryType(const VkPhysicalDeviceMemoryProperties& mp, u32 bits, VkMemoryPropertyFlags required,
                   VkMemoryPropertyFlags preferred, VkMemoryPropertyFlags avoid = 0) {
    u32 best = ~0u;
    i32 bestScore = -1;
    for (u32 i = 0; i < mp.memoryTypeCount; ++i) {
        if (!(bits & (1u << i))) continue;
        const VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
        if ((f & required) != required) continue;
        i32 score = 1;
        if ((f & preferred) == preferred) score += 4;
        if (!(f & avoid)) score += 2;
        if (score > bestScore) {
            bestScore = score;
            best = i;
        }
    }
    return best;
}

struct ExternalBufferSupport {
    bool exportable = false;
    bool importable = false;
    bool dedicatedOnly = false;
};

ExternalBufferSupport externalBufferSupport(VkPhysicalDevice pd, VkExternalMemoryHandleTypeFlagBits type) {
    VkPhysicalDeviceExternalBufferInfo bi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO};
    bi.usage = kTransferBufferUsage;
    bi.handleType = type;
    VkExternalBufferProperties p{VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
    vkGetPhysicalDeviceExternalBufferProperties(pd, &bi, &p);
    const VkExternalMemoryFeatureFlags f = p.externalMemoryProperties.externalMemoryFeatures;
    return {(f & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0, (f & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0,
            (f & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0};
}

u64 hostPointerAlignment(VkPhysicalDevice pd) {
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &hp;
    vkGetPhysicalDeviceProperties2(pd, &p2);
    return hp.minImportedHostPointerAlignment;
}

void* hostAlloc(u64 size, u64 alignment) {
#ifdef _WIN32
    return _aligned_malloc(size_t(size), size_t(alignment));
#else
    return std::aligned_alloc(size_t(alignment), size_t(size));
#endif
}

void hostFree(void* p) {
#ifdef _WIN32
    _aligned_free(p);
#else
    std::free(p);
#endif
}

AdapterInfo describeAdapter(VkPhysicalDevice pd, u32 index, VkPhysicalDevice primary) {
    AdapterInfo a;
    a.index = index;
    a.primary = pd == primary;
    VkPhysicalDeviceIDProperties idp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &idp;
    vkGetPhysicalDeviceProperties2(pd, &p2);
    const VkPhysicalDeviceProperties& p = p2.properties;
    a.name = p.deviceName;
    a.type = p.deviceType;
    a.vendorId = p.vendorID;
    a.deviceId = p.deviceID;
    a.apiVersion = p.apiVersion;
    std::memcpy(a.deviceUuid.data(), idp.deviceUUID, VK_UUID_SIZE);
    std::memcpy(a.driverUuid.data(), idp.driverUUID, VK_UUID_SIZE);
    std::memcpy(a.luid.data(), idp.deviceLUID, VK_LUID_SIZE);
    a.luidValid = idp.deviceLUIDValid == VK_TRUE;
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (u32 h = 0; h < mp.memoryHeapCount; ++h) {
        if (mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) a.deviceLocalBytes += mp.memoryHeaps[h].size;
    }
    const auto exts = deviceExtensionList(pd);
    a.externalMemoryHost = hasExt(exts, kExtMemHost);
    a.externalMemoryOpaque = hasExt(exts, kExtMemOpaque);

    // Requirements of a secondary device: what the bindless compute path and the timeline sync use.
    const bool v13 = VK_API_VERSION_MINOR(p.apiVersion) >= 3 || VK_API_VERSION_MAJOR(p.apiVersion) > 1;
    if (VK_API_VERSION_MAJOR(p.apiVersion) == 1 && VK_API_VERSION_MINOR(p.apiVersion) < 2) {
        a.unsuitableReason = "Vulkan 1.2 required";
        return a;
    }
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceSynchronization2Features fs2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f12;
    const bool extSync2 = !v13 && hasExt(exts, kExtSync2);
    if (v13) f12.pNext = &f13;
    else if (extSync2) f12.pNext = &fs2;
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    std::string missing;
    auto need = [&](VkBool32 v, const char* n) {
        if (!v) missing += std::string(missing.empty() ? "" : ", ") + n;
    };
    need(f12.timelineSemaphore, "timelineSemaphore");
    need(f12.descriptorIndexing, "descriptorIndexing");
    need(f12.runtimeDescriptorArray, "runtimeDescriptorArray");
    need(f12.descriptorBindingPartiallyBound, "descriptorBindingPartiallyBound");
    need(f12.descriptorBindingSampledImageUpdateAfterBind, "sampledImageUpdateAfterBind");
    need(f12.descriptorBindingStorageImageUpdateAfterBind, "storageImageUpdateAfterBind");
    need(f12.shaderSampledImageArrayNonUniformIndexing, "sampledImageArrayNonUniformIndexing");
    need(f12.bufferDeviceAddress, "bufferDeviceAddress");
    need(f12.scalarBlockLayout, "scalarBlockLayout");
    need(v13 ? f13.synchronization2 : extSync2 ? fs2.synchronization2 : VK_FALSE, "synchronization2");
    u32 familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> fams(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &familyCount, fams.data());
    const bool compute = std::any_of(fams.begin(), fams.end(), [](const VkQueueFamilyProperties& q) {
        return (q.queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
    });
    need(compute ? VK_TRUE : VK_FALSE, "compute queue");
    if (!missing.empty()) {
        a.unsuitableReason = "missing " + missing;
        return a;
    }
    a.suitable = true;
    return a;
}

std::string hexBytes(const u8* b, usize n) {
    std::string s;
    for (usize i = 0; i < n; ++i) s += std::format("{:02x}", b[i]);
    return s;
}

const char* kCalibrationShader = R"(#version 460
#include <common/bindless.glsl>
// Split rendering calibration: a post-process-like kernel (9 bilinear taps + ALU, RGBA8 in/out).
layout(local_size_x = 8, local_size_y = 8) in;
OX_PUSH_CONSTANTS({ uint src; uint dst; uint width; uint height; });
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= int(pc.width) || p.y >= int(pc.height)) return;
    vec2 rcp = 1.0 / vec2(pc.width, pc.height);
    vec2 uv = (vec2(p) + 0.5) * rcp;
    vec4 acc = vec4(0.0);
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec4 c = OX_SAMPLE_2D_LOD(pc.src, OX_SAMPLER_LINEAR_CLAMP, uv + vec2(x, y) * rcp, 0.0);
            acc += c * (1.0 + 0.1 * dot(c.rgb, vec3(0.299, 0.587, 0.114)));
        }
    }
    vec4 v = acc / 9.0;
    for (int i = 0; i < 24; ++i) v = fract(v * 1.0001 + 0.0001);
    OX_IMAGE_STORE_2D(rgba8, pc.dst, p, v);
}
)";

} // namespace

const char* adapterTypeName(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
    default: return "other";
    }
}

std::vector<AdapterInfo> enumerateAdapters(const Device& primary) {
    std::vector<AdapterInfo> out;
    u32 n = 0;
    vkEnumeratePhysicalDevices(primary.vkInstance(), &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(primary.vkInstance(), &n, pds.data());
    for (u32 i = 0; i < n; ++i) out.push_back(describeAdapter(pds[i], i, primary.vkPhysicalDevice()));
    return out;
}

std::string describeAdapters(const std::vector<AdapterInfo>& adapters) {
    std::string s;
    for (const AdapterInfo& a : adapters) {
        s += std::format("[{}] {} ({}, vendor 0x{:04x}, Vulkan {}.{}, {} MiB local){}{} ext-host {} ext-opaque {} LUID {}{}\n",
                         a.index, a.name, adapterTypeName(a.type), a.vendorId, VK_API_VERSION_MAJOR(a.apiVersion),
                         VK_API_VERSION_MINOR(a.apiVersion), a.deviceLocalBytes >> 20, a.primary ? " primary" : "",
                         a.suitable ? "" : (" unsuitable: " + a.unsuitableReason), a.externalMemoryHost ? "yes" : "no",
                         a.externalMemoryOpaque ? "yes" : "no",
                         a.luidValid ? hexBytes(a.luid.data(), a.luid.size()) : std::string("-"), "");
    }
    return s;
}

void appendMultiDeviceExtensions(DeviceDesc& desc) {
    for (const char* e : {kExtMemOpaque, kExtMemHost}) {
        if (std::find(desc.optionalDeviceExtensions.begin(), desc.optionalDeviceExtensions.end(), e) ==
            desc.optionalDeviceExtensions.end()) {
            desc.optionalDeviceExtensions.emplace_back(e);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Implementation structures

struct SecondaryTexture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    u32 sampled = kInvalidBindlessIndex;
    u32 storage = kInvalidBindlessIndex;
    TextureDesc desc;
};

struct ResolvedDispatch {
    VkPipeline pipeline = VK_NULL_HANDLE;
    std::array<u8, kMaxPushConstantSize> push{};
    u32 pushSize = 0;
    std::array<u32, 3> groups{1, 1, 1};
};

struct Job {
    CrossDeviceChannel::Impl* channel = nullptr;
    u32 slot = 0;
    u64 ticket = 0;
    VkSemaphore primaryTimeline = VK_NULL_HANDLE;
    u64 primaryValue = 0;
    std::vector<ResolvedDispatch> dispatches;
    CrossDeviceJobTiming timing;
};

struct SecondaryDevice::Impl {
    Device* primary = nullptr;
    VkDevice primaryDevice = VK_NULL_HANDLE;
    AdapterInfo adapter;
    CrossDeviceCaps caps;
    TransferPathChoice transfer;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memProps{};
    VkPhysicalDeviceMemoryProperties primaryMemProps{};
    VkPhysicalDeviceProperties props{};
    VkDevice device = VK_NULL_HANDLE;
    VolkDeviceTable vk{};
    PFN_vkCmdPipelineBarrier2 cmdBarrier2 = nullptr;
    PFN_vkCmdWriteTimestamp2 cmdTimestamp2 = nullptr;
    PFN_vkQueueSubmit2 queueSubmit2 = nullptr;
    u32 family = 0;
    VkQueue queue = VK_NULL_HANDLE;
    u32 timestampBits = 0;
    bool enabledHostPointer = false;
    bool enabledOpaque = false;

    // Bindless heap (same layout as the primary's set 0, so the same SPIR-V runs on both devices).
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    IndexAllocator sampledIndices, storageIndices;
    std::vector<VkSampler> samplers;
    std::mutex resourceMutex; // descriptor writes, index allocation, pipelines

    struct PipelineEntry {
        u32 version = ~0u;
        VkPipeline pipeline = VK_NULL_HANDLE;
        std::array<u32, 3> localSize{1, 1, 1};
        std::string error;
    };
    std::unordered_map<u64, PipelineEntry> pipelines;
    std::vector<VkPipeline> retiredPipelines;

    // Worker
    std::thread worker;
    std::mutex jobMutex;
    std::condition_variable jobCv, doneCv;
    std::deque<Job> jobs;
    bool stop = false;
    std::atomic<bool> lost{false};
    std::atomic<bool> simulateLoss{false};
    std::string lostReason; // jobMutex
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkSemaphore timeline = VK_NULL_HANDLE;
    u64 timelineValue = 0;
    VkQueryPool queries = VK_NULL_HANDLE;

    f32 speedRatio = 0.0f;
    f64 calPrimaryMs = 0.0, calSecondaryMs = 0.0;

    ~Impl();
    bool init(Device& primaryDevice, const SecondaryDeviceDesc& desc, std::string& error);
    bool createBindless(u32 maxTextures);
    bool resolvePipeline(PipelineHandle h, PipelineEntry& out, std::string& error);
    bool createTexture(const TextureDesc& desc, VkImageUsageFlags usage, SecondaryTexture& out, std::string& error);
    void destroyTexture(SecondaryTexture& t);
    bool submitAndWait(u64 timeoutNs, VkResult& result);
    void markLost(std::string reason);
    void workerMain();
    bool runJob(Job& job);
    void calibrate();
    void barrier(VkCommandBuffer cb, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                 VkAccessFlags2 dstAccess, std::span<const VkImageMemoryBarrier2> images = {},
                 std::span<const VkBufferMemoryBarrier2> buffers = {});
};

struct ChannelSide {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    u8* mapped = nullptr;
};

struct CrossDeviceChannel::Impl {
    SecondaryDevice::Impl* dev = nullptr;
    CrossDeviceChannelDesc desc;
    CrossDeviceTransfer path = CrossDeviceTransfer::Staging;
    struct Region {
        u32 texture = 0;
        u64 offset = 0;
        u64 size = 0;
    };
    std::vector<Region> up, down; // per direction, offsets within one slot
    u64 upSlot = 0, downSlot = 0; // slot strides
    u64 upBytes = 0, downBytes = 0;
    std::vector<SecondaryTexture> textures;
    // [0] = uplink (primary -> secondary), [1] = downlink. ExternalMemory/HostPointer: both sides share the payload.
    ChannelSide primarySide[2], secondarySide[2];
    void* hostMemory[2] = {nullptr, nullptr};
    u32 primaryFamily = 0;

    struct Slot {
        enum class State : u8 { Empty, Queued, Done, Failed };
        u64 ticket = ~0ull;
        State state = State::Empty;
        CrossDeviceJobTiming timing;
    };
    std::vector<Slot> slots; // dev->jobMutex
    u32 pending = 0;          // dev->jobMutex

    bool create(std::string& error);
    void destroy();
    bool createSharedOpaque(u32 dir, u64 size, std::string& error);
    bool createSharedHost(u32 dir, u64 size, std::string& error);
    bool createStaging(u32 dir, u64 size, std::string& error);
};

// ---------------------------------------------------------------------------------------------------------------
// SecondaryDevice

SecondaryDevice::SecondaryDevice() : m_impl(std::make_unique<Impl>()) {}

SecondaryDevice::~SecondaryDevice() {
    if (m_impl->device) {
        OX_LOG_INFO("rhi", "split: destroying secondary device '{}'", m_impl->adapter.name);
    }
    m_impl.reset();
    g_liveSecondary.fetch_sub(1);
}

u32 SecondaryDevice::liveCount() { return g_liveSecondary.load(); }

std::unique_ptr<SecondaryDevice> SecondaryDevice::create(Device& primary, const SecondaryDeviceDesc& desc,
                                                         std::string* error) {
    std::unique_ptr<SecondaryDevice> d(new SecondaryDevice());
    g_liveSecondary.fetch_add(1);
    std::string err;
    if (!d->m_impl->init(primary, desc, err)) {
        if (error) *error = err;
        OX_LOG_INFO("rhi", "split: no secondary device: {}", err);
        return nullptr;
    }
    return d;
}

const AdapterInfo& SecondaryDevice::adapter() const { return m_impl->adapter; }
const CrossDeviceCaps& SecondaryDevice::caps() const { return m_impl->caps; }
CrossDeviceTransfer SecondaryDevice::transferPath() const { return m_impl->transfer.path; }
const std::string& SecondaryDevice::transferReason() const { return m_impl->transfer.reason; }
bool SecondaryDevice::sameAdapter() const { return m_impl->adapter.primary; }
f32 SecondaryDevice::calibratedSpeedRatio() const { return m_impl->speedRatio; }
f64 SecondaryDevice::calibrationPrimaryMs() const { return m_impl->calPrimaryMs; }
f64 SecondaryDevice::calibrationSecondaryMs() const { return m_impl->calSecondaryMs; }
VkDevice SecondaryDevice::vkDevice() const { return m_impl->device; }
Device& SecondaryDevice::primary() const { return *m_impl->primary; }
bool SecondaryDevice::lost() const { return m_impl->lost.load(); }
std::string SecondaryDevice::lostReason() const {
    std::lock_guard lock(m_impl->jobMutex);
    return m_impl->lostReason;
}
void SecondaryDevice::simulateDeviceLost() { m_impl->simulateLoss = true; }

void SecondaryDevice::waitIdle() {
    {
        std::unique_lock lock(m_impl->jobMutex);
        m_impl->doneCv.wait(lock, [&] { return m_impl->jobs.empty(); });
    }
    if (m_impl->device) m_impl->vk.vkDeviceWaitIdle(m_impl->device);
}

bool SecondaryDevice::preparePipeline(PipelineHandle primaryPipeline, std::string* error) {
    Impl::PipelineEntry e;
    std::string err;
    const bool ok = m_impl->resolvePipeline(primaryPipeline, e, err);
    if (!ok && error) *error = err;
    return ok;
}

bool SecondaryDevice::Impl::init(Device& primaryDev, const SecondaryDeviceDesc& desc, std::string& error) {
    primary = &primaryDev;
    primaryDevice = primaryDev.vkDevice();
    vkGetPhysicalDeviceMemoryProperties(primaryDev.vkPhysicalDevice(), &primaryMemProps);

    // ---- adapter selection ----
    const std::vector<AdapterInfo> adapters = enumerateAdapters(primaryDev);
    const AdapterInfo* chosen = nullptr;
    if (desc.adapterIndex >= 0) {
        for (const AdapterInfo& a : adapters) {
            if (a.index == u32(desc.adapterIndex)) chosen = &a;
        }
        if (!chosen) {
            error = std::format("adapter {} does not exist ({} adapters)", desc.adapterIndex, adapters.size());
            return false;
        }
        if (chosen->primary && !desc.allowSameAdapter) {
            error = std::format("adapter {} is the primary GPU (set r.SplitRendering.AllowSameAdapter 1 to test on it)",
                                chosen->index);
            return false;
        }
    } else {
        auto rank = [](const AdapterInfo& a) {
            return a.type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 0 : a.type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 1 : 2;
        };
        for (const AdapterInfo& a : adapters) {
            if (a.primary || !a.suitable || a.type == VK_PHYSICAL_DEVICE_TYPE_CPU) continue; // no software rasterizers
            if (!chosen || rank(a) < rank(*chosen)) chosen = &a;
        }
        if (!chosen && desc.allowSameAdapter) {
            for (const AdapterInfo& a : adapters) {
                if (a.primary) chosen = &a;
            }
        }
        if (!chosen) {
            error = std::format("no second GPU is visible to Vulkan ({} adapter(s): {})", adapters.size(),
                                adapters.empty() ? std::string("none") : adapters.front().name);
            return false;
        }
    }
    if (!chosen->suitable) {
        error = std::format("adapter '{}' is unsuitable: {}", chosen->name, chosen->unsuitableReason);
        return false;
    }
    adapter = *chosen;
    {
        u32 n = 0;
        vkEnumeratePhysicalDevices(primaryDev.vkInstance(), &n, nullptr);
        std::vector<VkPhysicalDevice> pds(n);
        vkEnumeratePhysicalDevices(primaryDev.vkInstance(), &n, pds.data());
        physical = pds.at(adapter.index);
    }
    vkGetPhysicalDeviceProperties(physical, &props);
    vkGetPhysicalDeviceMemoryProperties(physical, &memProps);

    // ---- queue: prefer a compute family without graphics (async compute on the same GPU in test mode) ----
    u32 familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> fams(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, fams.data());
    i32 fam = -1;
    for (u32 i = 0; i < familyCount; ++i) {
        if ((fams[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && !(fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            fams[i].timestampValidBits > 0) {
            fam = i32(i);
            break;
        }
    }
    for (u32 i = 0; i < familyCount && fam < 0; ++i) {
        if (fams[i].queueFlags & VK_QUEUE_COMPUTE_BIT) fam = i32(i);
    }
    family = u32(fam);
    timestampBits = fams[family].timestampValidBits;

    // ---- extensions + features ----
    const auto exts = deviceExtensionList(physical);
    const bool v13 = VK_API_VERSION_MINOR(props.apiVersion) >= 3 || VK_API_VERSION_MAJOR(props.apiVersion) > 1;
    std::vector<const char*> enable;
    if (!v13) enable.push_back(kExtSync2);
    if (hasExt(exts, kExtMemOpaque)) {
        enable.push_back(kExtMemOpaque);
        enabledOpaque = true;
    }
    if (hasExt(exts, kExtMemHost)) {
        enable.push_back(kExtMemHost);
        enabledHostPointer = true;
    }

    VkPhysicalDeviceVulkan11Features s11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features s12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features s13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 sup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    sup.pNext = &s11;
    s11.pNext = &s12;
    if (v13) s12.pNext = &s13;
    vkGetPhysicalDeviceFeatures2(physical, &sup);

    VkPhysicalDeviceFeatures e10{};
    e10.samplerAnisotropy = sup.features.samplerAnisotropy;
    e10.shaderInt64 = sup.features.shaderInt64;
    e10.shaderInt16 = sup.features.shaderInt16;
    e10.shaderStorageImageReadWithoutFormat = sup.features.shaderStorageImageReadWithoutFormat;
    e10.shaderStorageImageWriteWithoutFormat = sup.features.shaderStorageImageWriteWithoutFormat;
    e10.shaderStorageImageExtendedFormats = sup.features.shaderStorageImageExtendedFormats;
    e10.shaderImageGatherExtended = sup.features.shaderImageGatherExtended;
    e10.imageCubeArray = sup.features.imageCubeArray;
    VkPhysicalDeviceVulkan11Features e11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    e11.storageBuffer16BitAccess = s11.storageBuffer16BitAccess;
    e11.uniformAndStorageBuffer16BitAccess = s11.uniformAndStorageBuffer16BitAccess;
    e11.shaderDrawParameters = s11.shaderDrawParameters;
    VkPhysicalDeviceVulkan12Features e12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    e12.timelineSemaphore = VK_TRUE;
    e12.descriptorIndexing = VK_TRUE;
    e12.runtimeDescriptorArray = VK_TRUE;
    e12.descriptorBindingPartiallyBound = VK_TRUE;
    e12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    e12.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
    e12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    e12.bufferDeviceAddress = VK_TRUE;
    e12.scalarBlockLayout = VK_TRUE;
    e12.shaderStorageImageArrayNonUniformIndexing = s12.shaderStorageImageArrayNonUniformIndexing;
    e12.shaderFloat16 = s12.shaderFloat16;
    e12.shaderInt8 = s12.shaderInt8;
    e12.storageBuffer8BitAccess = s12.storageBuffer8BitAccess;
    e12.uniformAndStorageBuffer8BitAccess = s12.uniformAndStorageBuffer8BitAccess;
    e12.vulkanMemoryModel = s12.vulkanMemoryModel;
    e12.vulkanMemoryModelDeviceScope = s12.vulkanMemoryModelDeviceScope;
    e12.shaderSubgroupExtendedTypes = s12.shaderSubgroupExtendedTypes;
    e12.hostQueryReset = s12.hostQueryReset;
    e12.samplerFilterMinmax = s12.samplerFilterMinmax;
    e12.uniformBufferStandardLayout = s12.uniformBufferStandardLayout;
    e12.descriptorBindingUpdateUnusedWhilePending = s12.descriptorBindingUpdateUnusedWhilePending;
    VkPhysicalDeviceVulkan13Features e13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    e13.synchronization2 = VK_TRUE;
    e13.maintenance4 = s13.maintenance4;
    e13.shaderDemoteToHelperInvocation = s13.shaderDemoteToHelperInvocation;
    e13.shaderIntegerDotProduct = s13.shaderIntegerDotProduct;
    e13.subgroupSizeControl = s13.subgroupSizeControl;
    e13.computeFullSubgroups = s13.computeFullSubgroups;
    e13.shaderTerminateInvocation = s13.shaderTerminateInvocation;
    VkPhysicalDeviceSynchronization2Features eS2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
    eS2.synchronization2 = VK_TRUE;
    VkPhysicalDeviceFeatures2 en{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    en.features = e10;
    en.pNext = &e11;
    e11.pNext = &e12;
    e12.pNext = v13 ? static_cast<void*>(&e13) : static_cast<void*>(&eS2);

    const f32 priority = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &en;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qi;
    dci.enabledExtensionCount = u32(enable.size());
    dci.ppEnabledExtensionNames = enable.data();
    if (VkResult r = vkCreateDevice(physical, &dci, nullptr, &device); r != VK_SUCCESS) {
        error = std::format("vkCreateDevice on '{}': {}", adapter.name, vkResultName(r));
        device = VK_NULL_HANDLE;
        return false;
    }
    volkLoadDeviceTable(&vk, device);
    cmdBarrier2 = vk.vkCmdPipelineBarrier2 ? vk.vkCmdPipelineBarrier2 : vk.vkCmdPipelineBarrier2KHR;
    cmdTimestamp2 = vk.vkCmdWriteTimestamp2 ? vk.vkCmdWriteTimestamp2 : vk.vkCmdWriteTimestamp2KHR;
    queueSubmit2 = vk.vkQueueSubmit2 ? vk.vkQueueSubmit2 : vk.vkQueueSubmit2KHR;
    if (!cmdBarrier2 || !cmdTimestamp2 || !queueSubmit2) {
        error = "synchronization2 entry points missing";
        return false;
    }
    vk.vkGetDeviceQueue(device, family, 0, &queue);

    // ---- what the two devices can share ----
    {
        VkPhysicalDeviceIDProperties pid{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &pid;
        vkGetPhysicalDeviceProperties2(primaryDev.vkPhysicalDevice(), &p2);
        caps.sameDevice = std::memcmp(pid.deviceUUID, adapter.deviceUuid.data(), VK_UUID_SIZE) == 0 &&
                          std::memcmp(pid.driverUUID, adapter.driverUuid.data(), VK_UUID_SIZE) == 0;
        caps.luidMatch = pid.deviceLUIDValid && adapter.luidValid &&
                         std::memcmp(pid.deviceLUID, adapter.luid.data(), VK_LUID_SIZE) == 0;
    }
    const ExternalBufferSupport pOpaque = externalBufferSupport(primaryDev.vkPhysicalDevice(), kOpaqueHandle);
    const ExternalBufferSupport sOpaque = externalBufferSupport(physical, kOpaqueHandle);
    caps.primaryExportOpaque = primaryDev.hasDeviceExtension(kExtMemOpaque) && pOpaque.exportable;
    caps.secondaryImportOpaque = enabledOpaque && sOpaque.importable;
    const ExternalBufferSupport pHost = externalBufferSupport(primaryDev.vkPhysicalDevice(), kHostHandle);
    const ExternalBufferSupport sHost = externalBufferSupport(physical, kHostHandle);
    caps.primaryHostPointer = primaryDev.hasDeviceExtension(kExtMemHost) && pHost.importable && vkGetMemoryHostPointerPropertiesEXT;
    caps.secondaryHostPointer = enabledHostPointer && sHost.importable && vk.vkGetMemoryHostPointerPropertiesEXT;
    if (caps.primaryHostPointer || caps.secondaryHostPointer) {
        caps.hostPointerAlignment = std::max(hostPointerAlignment(primaryDev.vkPhysicalDevice()), hostPointerAlignment(physical));
    }
    transfer = selectTransferPath(caps, desc.transfer);

    // ---- bindless heap, worker objects ----
    if (!createBindless(desc.maxTextures)) {
        error = "secondary bindless heap creation failed";
        return false;
    }
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family;
    OX_VK_CHECK(vk.vkCreateCommandPool(device, &pci, nullptr, &cmdPool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = cmdPool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    OX_VK_CHECK(vk.vkAllocateCommandBuffers(device, &cai, &cmd));
    VkSemaphoreTypeCreateInfo ti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    si.pNext = &ti;
    OX_VK_CHECK(vk.vkCreateSemaphore(device, &si, nullptr, &timeline));
    if (timestampBits > 0) {
        VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpi.queryCount = kQueriesPerJob;
        OX_VK_CHECK(vk.vkCreateQueryPool(device, &qpi, nullptr, &queries));
    }

    OX_LOG_INFO("rhi", "split: secondary device '{}' ({}{}), queue family {}, transfer {} ({})", adapter.name,
                adapterTypeName(adapter.type), adapter.primary ? ", SAME ADAPTER test mode" : "", family,
                crossDeviceTransferName(transfer.path), transfer.reason);
    if (desc.calibrate) calibrate();
    worker = std::thread([this] { workerMain(); });
    return true;
}

bool SecondaryDevice::Impl::createBindless(u32 maxTextures) {
    const u32 count = std::max(16u, maxTextures);
    const u32 samplerCount = 16;
    sampledIndices.reset(count);
    storageIndices.reset(count);
    VkDescriptorSetLayoutBinding b[3]{};
    b[0] = {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, count, VK_SHADER_STAGE_ALL, nullptr};
    b[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, count, VK_SHADER_STAGE_ALL, nullptr};
    b[2] = {2, VK_DESCRIPTOR_TYPE_SAMPLER, samplerCount, VK_SHADER_STAGE_ALL, nullptr};
    const VkDescriptorBindingFlags f = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    const VkDescriptorBindingFlags flags[3] = {f, f, f};
    VkDescriptorSetLayoutBindingFlagsCreateInfo fi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    fi.bindingCount = 3;
    fi.pBindingFlags = flags;
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.pNext = &fi;
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    li.bindingCount = 3;
    li.pBindings = b;
    if (vk.vkCreateDescriptorSetLayout(device, &li, nullptr, &setLayout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, count},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, count},
                                     {VK_DESCRIPTOR_TYPE_SAMPLER, samplerCount}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pi.maxSets = 1;
    pi.poolSizeCount = 3;
    pi.pPoolSizes = sizes;
    if (vk.vkCreateDescriptorPool(device, &pi, nullptr, &pool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &setLayout;
    if (vk.vkAllocateDescriptorSets(device, &ai, &set) != VK_SUCCESS) return false;
    VkPushConstantRange pc{VK_SHADER_STAGE_ALL, 0, std::min(kMaxPushConstantSize, props.limits.maxPushConstantsSize)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &setLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pc;
    if (vk.vkCreatePipelineLayout(device, &pl, nullptr, &layout) != VK_SUCCESS) return false;

    // DefaultSampler indices 0..5 are constants in bindless.glsl: mirror the primary's samplers.
    VkPhysicalDeviceFeatures f10{};
    vkGetPhysicalDeviceFeatures(physical, &f10);
    for (u32 i = 0; i < u32(DefaultSampler::Count); ++i) {
        SamplerDesc d;
        {
            DeviceState& ps = primary->state();
            std::lock_guard lock(ps.resourceMutex);
            d = ps.samplers.at(primary->defaultSampler(DefaultSampler(i))).desc;
        }
        VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        ci.magFilter = d.magFilter;
        ci.minFilter = d.minFilter;
        ci.mipmapMode = d.mipmapMode;
        ci.addressModeU = d.addressU;
        ci.addressModeV = d.addressV;
        ci.addressModeW = d.addressW;
        ci.mipLodBias = d.mipLodBias;
        ci.anisotropyEnable = d.maxAnisotropy > 1.0f && f10.samplerAnisotropy;
        ci.maxAnisotropy = ci.anisotropyEnable ? std::min(d.maxAnisotropy, props.limits.maxSamplerAnisotropy) : 1.0f;
        ci.compareEnable = d.compareEnable;
        ci.compareOp = d.compareOp;
        ci.minLod = d.minLod;
        ci.maxLod = d.maxLod;
        ci.borderColor = d.borderColor;
        VkSampler s = VK_NULL_HANDLE;
        if (vk.vkCreateSampler(device, &ci, nullptr, &s) != VK_SUCCESS) return false;
        samplers.push_back(s);
        VkDescriptorImageInfo ii{s, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = 2;
        w.dstArrayElement = i;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        w.pImageInfo = &ii;
        vk.vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
    }
    return true;
}

SecondaryDevice::Impl::~Impl() {
    {
        std::lock_guard lock(jobMutex);
        stop = true;
    }
    jobCv.notify_all();
    if (worker.joinable()) worker.join();
    if (!device) return;
    vk.vkDeviceWaitIdle(device);
    for (auto& [k, e] : pipelines) {
        if (e.pipeline) vk.vkDestroyPipeline(device, e.pipeline, nullptr);
    }
    for (VkPipeline p : retiredPipelines) vk.vkDestroyPipeline(device, p, nullptr);
    for (VkSampler s : samplers) vk.vkDestroySampler(device, s, nullptr);
    if (layout) vk.vkDestroyPipelineLayout(device, layout, nullptr);
    if (pool) vk.vkDestroyDescriptorPool(device, pool, nullptr);
    if (setLayout) vk.vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    if (queries) vk.vkDestroyQueryPool(device, queries, nullptr);
    if (timeline) vk.vkDestroySemaphore(device, timeline, nullptr);
    if (cmdPool) vk.vkDestroyCommandPool(device, cmdPool, nullptr);
    vk.vkDestroyDevice(device, nullptr);
    device = VK_NULL_HANDLE;
}

bool SecondaryDevice::Impl::resolvePipeline(PipelineHandle h, PipelineEntry& out, std::string& error) {
    if (!primary->isAlive(h)) {
        error = "primary pipeline handle is not alive";
        return false;
    }
    if (primary->pipelineKind(h) != PipelineKind::Compute) {
        error = "only compute pipelines are portable";
        return false;
    }
    const u32 version = primary->pipelineVersion(h);
    std::lock_guard lock(resourceMutex);
    PipelineEntry& e = pipelines[h.packed()];
    if (e.version == version && (e.pipeline || !e.error.empty())) {
        out = e;
        if (!e.pipeline) error = e.error;
        return e.pipeline != VK_NULL_HANDLE;
    }
    ComputePipelineDesc cd;
    std::array<u32, 3> localSize{1, 1, 1};
    {
        DeviceState& ps = primary->state();
        std::lock_guard plock(ps.resourceMutex);
        const PipelineRecord& rec = ps.pipelines.at(h);
        cd = rec.compute;
        localSize = rec.localSize;
    }
    std::vector<u32> words = cd.shader.spirv;
    if (words.empty()) {
        ShaderCompileDesc sc;
        sc.path = cd.shader.path;
        sc.source = cd.shader.source;
        sc.stage = ShaderStage::Compute;
        sc.entryPoint = cd.shader.entryPoint;
        sc.defines = cd.shader.defines;
        ShaderCompileResult res = primary->shaderCompiler().compile(sc);
        if (!res.success) {
            e.version = version;
            e.error = std::format("'{}': {}", cd.name, res.errors);
            error = e.error;
            return false;
        }
        words = std::move(res.spirv);
        const auto& ls = res.reflection.localSize;
        localSize = {std::max(1u, ls[0]), std::max(1u, ls[1]), std::max(1u, ls[2])};
    }
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = words.size() * 4;
    mi.pCode = words.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (VkResult r = vk.vkCreateShaderModule(device, &mi, nullptr, &module); r != VK_SUCCESS) {
        e.version = version;
        e.error = std::format("'{}': vkCreateShaderModule: {}", cd.name, vkResultName(r));
        error = e.error;
        return false;
    }
    std::vector<VkSpecializationMapEntry> entries;
    std::vector<u32> data;
    for (u32 i = 0; i < cd.specialization.size(); ++i) {
        entries.push_back({cd.specialization[i].id, i * 4, 4});
        data.push_back(cd.specialization[i].value);
    }
    VkSpecializationInfo spec{u32(entries.size()), entries.data(), data.size() * 4, data.data()};
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName = cd.shader.entryPoint.c_str();
    ci.stage.pSpecializationInfo = entries.empty() ? nullptr : &spec;
    ci.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult r = vk.vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline);
    vk.vkDestroyShaderModule(device, module, nullptr);
    if (r != VK_SUCCESS) {
        e.version = version;
        e.error = std::format("'{}': vkCreateComputePipelines: {}", cd.name, vkResultName(r));
        error = e.error;
        return false;
    }
    if (e.pipeline) retiredPipelines.push_back(e.pipeline); // a queued job may still use it (hot reload)
    e.pipeline = pipeline;
    e.version = version;
    e.localSize = localSize;
    e.error.clear();
    out = e;
    return true;
}

bool SecondaryDevice::Impl::createTexture(const TextureDesc& d, VkImageUsageFlags usage, SecondaryTexture& out,
                                          std::string& error) {
    if (d.type != TextureType::Tex2D || d.mipLevels != 1 || d.arrayLayers != 1 || d.samples > 1 || d.depth != 1 ||
        formatInfo(d.format).depth || formatInfo(d.format).compressed) {
        error = std::format("'{}': only single-mip, single-layer 2D colour textures are portable", d.name);
        return false;
    }
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(physical, d.format, &fp);
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) need |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if (usage & VK_IMAGE_USAGE_STORAGE_BIT) need |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    if ((fp.optimalTilingFeatures & need) != need) {
        error = std::format("'{}': format {} lacks features 0x{:x} on '{}'", d.name, formatName(d.format),
                            need & ~fp.optimalTilingFeatures, adapter.name);
        return false;
    }
    out.desc = d;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = d.format;
    ii.extent = {d.width, d.height, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (VkResult r = vk.vkCreateImage(device, &ii, nullptr, &out.image); r != VK_SUCCESS) {
        error = std::format("'{}': vkCreateImage: {}", d.name, vkResultName(r));
        return false;
    }
    VkMemoryRequirements req{};
    vk.vkGetImageMemoryRequirements(device, out.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(memProps, req.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == ~0u || vk.vkAllocateMemory(device, &ai, nullptr, &out.memory) != VK_SUCCESS) {
        error = std::format("'{}': image memory allocation failed", d.name);
        destroyTexture(out);
        return false;
    }
    vk.vkBindImageMemory(device, out.image, out.memory, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = out.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = d.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    OX_VK_CHECK(vk.vkCreateImageView(device, &vi, nullptr, &out.view));
    std::lock_guard lock(resourceMutex);
    std::array<VkWriteDescriptorSet, 2> w{};
    std::array<VkDescriptorImageInfo, 2> info{};
    u32 n = 0;
    if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
        out.sampled = sampledIndices.allocate();
        info[n] = {VK_NULL_HANDLE, out.view, VK_IMAGE_LAYOUT_GENERAL};
        w[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[n].dstSet = set;
        w[n].dstBinding = 0;
        w[n].dstArrayElement = out.sampled;
        w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w[n].pImageInfo = &info[n];
        n += out.sampled != kInvalidBindlessIndex ? 1 : 0;
    }
    if (usage & VK_IMAGE_USAGE_STORAGE_BIT) {
        out.storage = storageIndices.allocate();
        info[n] = {VK_NULL_HANDLE, out.view, VK_IMAGE_LAYOUT_GENERAL};
        w[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[n].dstSet = set;
        w[n].dstBinding = 1;
        w[n].dstArrayElement = out.storage;
        w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[n].pImageInfo = &info[n];
        n += out.storage != kInvalidBindlessIndex ? 1 : 0;
    }
    if (((usage & VK_IMAGE_USAGE_SAMPLED_BIT) && out.sampled == kInvalidBindlessIndex) ||
        ((usage & VK_IMAGE_USAGE_STORAGE_BIT) && out.storage == kInvalidBindlessIndex)) {
        error = "secondary bindless heap exhausted";
        return false;
    }
    vk.vkUpdateDescriptorSets(device, n, w.data(), 0, nullptr);
    return true;
}

void SecondaryDevice::Impl::destroyTexture(SecondaryTexture& t) {
    {
        std::lock_guard lock(resourceMutex);
        if (t.sampled != kInvalidBindlessIndex) sampledIndices.free(t.sampled);
        if (t.storage != kInvalidBindlessIndex) storageIndices.free(t.storage);
    }
    if (t.view) vk.vkDestroyImageView(device, t.view, nullptr);
    if (t.image) vk.vkDestroyImage(device, t.image, nullptr);
    if (t.memory) vk.vkFreeMemory(device, t.memory, nullptr);
    t = {};
}

void SecondaryDevice::Impl::barrier(VkCommandBuffer cb, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                                    VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                                    std::span<const VkImageMemoryBarrier2> images,
                                    std::span<const VkBufferMemoryBarrier2> buffers) {
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = srcStage;
    mb.srcAccessMask = srcAccess;
    mb.dstStageMask = dstStage;
    mb.dstAccessMask = dstAccess;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = srcStage || dstStage ? 1 : 0;
    di.pMemoryBarriers = &mb;
    di.imageMemoryBarrierCount = u32(images.size());
    di.pImageMemoryBarriers = images.data();
    di.bufferMemoryBarrierCount = u32(buffers.size());
    di.pBufferMemoryBarriers = buffers.data();
    cmdBarrier2(cb, &di);
}

bool SecondaryDevice::Impl::submitAndWait(u64 timeoutNs, VkResult& result) {
    timelineValue += 1;
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = cmd;
    VkSemaphoreSubmitInfo sig{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    sig.semaphore = timeline;
    sig.value = timelineValue;
    sig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &sig;
    result = queueSubmit2(queue, 1, &si, VK_NULL_HANDLE);
    if (result != VK_SUCCESS) return false;
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &timeline;
    wi.pValues = &timelineValue;
    result = vk.vkWaitSemaphores(device, &wi, timeoutNs);
    return result == VK_SUCCESS;
}

void SecondaryDevice::Impl::markLost(std::string reason) {
    std::lock_guard lock(jobMutex);
    if (!lost) {
        OX_LOG_ERROR("rhi", "split: secondary device '{}' lost: {}", adapter.name, reason);
        lostReason = std::move(reason);
    }
    lost = true;
    doneCv.notify_all();
}

void SecondaryDevice::Impl::calibrate() {
    // Same kernel, same SPIR-V, same resolution on both devices; ratio of the GPU times (best of 3).
    constexpr u32 kW = 1920, kH = 1080;
    ComputePipelineDesc cd;
    cd.name = "split.calibrate";
    cd.shader = ShaderStageDesc::glsl(kCalibrationShader, ShaderStage::Compute, "split_calibrate.comp");
    const PipelineHandle pipe = primary->createComputePipeline(cd);
    if (!primary->vkPipeline(pipe)) {
        OX_LOG_WARN("rhi", "split: calibration kernel failed to compile: {}", primary->lastPipelineError());
        primary->destroy(pipe);
        return;
    }
    TextureDesc td;
    td.width = kW;
    td.height = kH;
    td.format = VK_FORMAT_R8G8B8A8_UNORM;
    td.usage = TextureUsage::Sampled | TextureUsage::TransferDst;
    td.name = "split.calibrate.src";
    const TextureHandle pSrc = primary->createTexture(td);
    td.usage = TextureUsage::Storage;
    td.name = "split.calibrate.dst";
    const TextureHandle pDst = primary->createTexture(td);

    f64 primaryBest = 1e30;
    if (primary->caps().timestampQueries) {
        VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpi.queryCount = 2;
        VkQueryPool qp = VK_NULL_HANDLE;
        OX_VK_CHECK(vkCreateQueryPool(primaryDevice, &qpi, nullptr, &qp));
        struct {
            u32 src, dst, w, h;
        } pc{primary->sampledIndex(pSrc), primary->storageIndex(pDst), kW, kH};
        for (u32 run = 0; run < 3; ++run) {
            primary->immediateSubmit([&](CommandList& c) {
                c.transition(pSrc, Access::SampledCompute, true);
                c.transition(pDst, Access::StorageWriteCompute, true);
                vkCmdResetQueryPool(c.vk(), qp, 0, 2);
                vkCmdWriteTimestamp2(c.vk(), VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, qp, 0);
                c.bindPipeline(pipe);
                c.pushConstants(pc);
                c.dispatchThreads(kW, kH);
                vkCmdWriteTimestamp2(c.vk(), VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, qp, 1);
            });
            u64 ts[2]{};
            if (vkGetQueryPoolResults(primaryDevice, qp, 0, 2, sizeof(ts), ts, sizeof(u64),
                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
                primaryBest = std::min(primaryBest, f64(ts[1] - ts[0]) * primary->caps().timestampPeriodNs * 1e-6);
            }
        }
        vkDestroyQueryPool(primaryDevice, qp, nullptr);
    }
    primary->destroy(pSrc);
    primary->destroy(pDst);

    f64 secondaryBest = 1e30;
    PipelineEntry entry;
    std::string err;
    SecondaryTexture sSrc, sDst;
    TextureDesc sd;
    sd.width = kW;
    sd.height = kH;
    sd.format = VK_FORMAT_R8G8B8A8_UNORM;
    sd.name = "split.calibrate";
    if (queries && resolvePipeline(pipe, entry, err) &&
        createTexture(sd, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, sSrc, err) &&
        createTexture(sd, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, sDst, err)) {
        struct {
            u32 src, dst, w, h;
        } pc{sSrc.sampled, sDst.storage, kW, kH};
        const f64 period = props.limits.timestampPeriod;
        for (u32 run = 0; run < 3; ++run) {
            vk.vkResetCommandBuffer(cmd, 0);
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vk.vkBeginCommandBuffer(cmd, &bi);
            vk.vkCmdResetQueryPool(cmd, queries, 0, 2);
            VkImageMemoryBarrier2 ib[2]{};
            for (u32 i = 0; i < 2; ++i) {
                ib[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                ib[i].srcStageMask = VK_PIPELINE_STAGE_2_NONE;
                ib[i].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                ib[i].dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
                ib[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                ib[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
                ib[i].srcQueueFamilyIndex = ib[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                ib[i].image = i == 0 ? sSrc.image : sDst.image;
                ib[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            }
            barrier(cmd, 0, 0, 0, 0, ib);
            cmdTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries, 0);
            vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, entry.pipeline);
            vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
            vk.vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(pc), &pc);
            vk.vkCmdDispatch(cmd, (kW + entry.localSize[0] - 1) / entry.localSize[0],
                             (kH + entry.localSize[1] - 1) / entry.localSize[1], 1);
            cmdTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries, 1);
            vk.vkEndCommandBuffer(cmd);
            VkResult r = VK_SUCCESS;
            if (!submitAndWait(5'000'000'000ull, r)) {
                OX_LOG_WARN("rhi", "split: calibration on the secondary failed: {}", vkResultName(r));
                break;
            }
            u64 ts[2]{};
            if (vk.vkGetQueryPoolResults(device, queries, 0, 2, sizeof(ts), ts, sizeof(u64),
                                         VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
                secondaryBest = std::min(secondaryBest, f64(ts[1] - ts[0]) * period * 1e-6);
            }
        }
    } else if (!err.empty()) {
        OX_LOG_WARN("rhi", "split: calibration skipped: {}", err);
    }
    destroyTexture(sSrc);
    destroyTexture(sDst);
    {
        // The calibration pipeline is not needed any more on either device.
        std::lock_guard lock(resourceMutex);
        if (auto it = pipelines.find(pipe.packed()); it != pipelines.end()) {
            if (it->second.pipeline) vk.vkDestroyPipeline(device, it->second.pipeline, nullptr);
            pipelines.erase(it);
        }
    }
    primary->destroy(pipe);
    if (primaryBest < 1e29 && secondaryBest < 1e29 && primaryBest > 0.0) {
        calPrimaryMs = primaryBest;
        calSecondaryMs = secondaryBest;
        speedRatio = f32(secondaryBest / primaryBest);
        OX_LOG_INFO("rhi", "split: calibration (1080p post kernel): primary {:.3f} ms, secondary {:.3f} ms -> ratio {:.2f}",
                    primaryBest, secondaryBest, speedRatio);
    }
}

void SecondaryDevice::Impl::workerMain() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(jobMutex);
            jobCv.wait(lock, [&] { return stop || !jobs.empty(); });
            if (stop && jobs.empty()) return;
            job = std::move(jobs.front());
            jobs.pop_front();
        }
        const bool ok = !stop && !lost && runJob(job);
        {
            std::lock_guard lock(jobMutex);
            CrossDeviceChannel::Impl::Slot& s = job.channel->slots[job.slot];
            if (s.ticket == job.ticket) {
                s.state = ok ? CrossDeviceChannel::Impl::Slot::State::Done : CrossDeviceChannel::Impl::Slot::State::Failed;
                s.timing = job.timing;
            }
            job.channel->pending -= 1;
        }
        doneCv.notify_all();
    }
}

bool SecondaryDevice::Impl::runJob(Job& job) {
    if (simulateLoss.exchange(false)) {
        markLost("simulated device loss (test)");
        return false;
    }
    CrossDeviceChannel::Impl& c = *job.channel;
    // 1. Wait until the primary submission that recorded the export retired (CPU wait on the primary timeline:
    //    timeline semaphores cannot be shared between devices of different vendors).
    if (job.primaryValue > 0) {
        VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wi.semaphoreCount = 1;
        wi.pSemaphores = &job.primaryTimeline;
        wi.pValues = &job.primaryValue;
        const Clock::time_point start = Clock::now();
        for (;;) {
            const VkResult r = vkWaitSemaphores(primaryDevice, &wi, 50'000'000ull);
            if (r == VK_SUCCESS) break;
            if (r != VK_TIMEOUT || msSince(start) > 20000.0) {
                OX_LOG_WARN("rhi", "split: waiting for the primary export failed: {}", vkResultName(r));
                return false;
            }
            std::lock_guard lock(jobMutex);
            if (stop) return false;
        }
    }
    const Clock::time_point exported = Clock::now();
    // 2. Staging: CPU copy primary readback -> secondary upload.
    if (c.path == CrossDeviceTransfer::Staging && c.upBytes > 0) {
        const Clock::time_point t = Clock::now();
        std::memcpy(c.secondarySide[0].mapped + job.slot * c.upSlot, c.primarySide[0].mapped + job.slot * c.upSlot,
                    size_t(c.upBytes));
        job.timing.cpuCopyMs += msSince(t);
    }
    // 3. Record: upload copies -> dispatches -> download copies (all textures in GENERAL, fully rewritten per job).
    vk.vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk.vkBeginCommandBuffer(cmd, &bi);
    const bool ts = queries != VK_NULL_HANDLE;
    if (ts) vk.vkCmdResetQueryPool(cmd, queries, 0, kQueriesPerJob);
    if (ts) cmdTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries, 0);
    std::vector<VkImageMemoryBarrier2> ib(c.textures.size());
    for (usize i = 0; i < c.textures.size(); ++i) {
        ib[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        ib[i].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; // previous job's readers/writers (WAR/WAW)
        ib[i].srcAccessMask = 0;
        ib[i].dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        ib[i].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        ib[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ib[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        ib[i].srcQueueFamilyIndex = ib[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib[i].image = c.textures[i].image;
        ib[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    VkBufferMemoryBarrier2 upAcquire{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    upAcquire.buffer = c.secondarySide[0].buffer;
    upAcquire.offset = job.slot * c.upSlot;
    upAcquire.size = c.upSlot;
    upAcquire.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    upAcquire.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    if (c.path == CrossDeviceTransfer::ExternalMemory) {
        upAcquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        upAcquire.dstQueueFamilyIndex = family;
    } else {
        upAcquire.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        upAcquire.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
        upAcquire.srcQueueFamilyIndex = upAcquire.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    }
    const bool hasUp = c.upBytes > 0;
    barrier(cmd, 0, 0, 0, 0, ib, hasUp ? std::span<const VkBufferMemoryBarrier2>(&upAcquire, 1) : std::span<const VkBufferMemoryBarrier2>{});
    for (const CrossDeviceChannel::Impl::Region& r : c.up) {
        const SecondaryTexture& t = c.textures[r.texture];
        VkBufferImageCopy bc{};
        bc.bufferOffset = job.slot * c.upSlot + r.offset;
        bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bc.imageExtent = {t.desc.width, t.desc.height, 1};
        vk.vkCmdCopyBufferToImage(cmd, c.secondarySide[0].buffer, t.image, VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
    }
    barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    if (ts) cmdTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries, 1);
    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    for (usize i = 0; i < job.dispatches.size(); ++i) {
        const ResolvedDispatch& d = job.dispatches[i];
        if (i > 0) {
            barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
        }
        vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.pipeline);
        if (d.pushSize) vk.vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, d.pushSize, d.push.data());
        vk.vkCmdDispatch(cmd, d.groups[0], d.groups[1], d.groups[2]);
    }
    if (ts) cmdTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries, 2);
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT);
    for (const CrossDeviceChannel::Impl::Region& r : c.down) {
        const SecondaryTexture& t = c.textures[r.texture];
        VkBufferImageCopy bc{};
        bc.bufferOffset = job.slot * c.downSlot + r.offset;
        bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bc.imageExtent = {t.desc.width, t.desc.height, 1};
        vk.vkCmdCopyImageToBuffer(cmd, t.image, VK_IMAGE_LAYOUT_GENERAL, c.secondarySide[1].buffer, 1, &bc);
    }
    if (c.downBytes > 0) {
        VkBufferMemoryBarrier2 release{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        release.buffer = c.secondarySide[1].buffer;
        release.offset = job.slot * c.downSlot;
        release.size = c.downSlot;
        release.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        release.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        if (c.path == CrossDeviceTransfer::ExternalMemory) {
            release.srcQueueFamilyIndex = family;
            release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        } else {
            release.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            release.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            release.srcQueueFamilyIndex = release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        }
        barrier(cmd, 0, 0, 0, 0, {}, std::span<const VkBufferMemoryBarrier2>(&release, 1));
    }
    if (ts) cmdTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries, 3);
    vk.vkEndCommandBuffer(cmd);

    // 4. Submit + wait (the worker owns the queue).
    VkResult r = VK_SUCCESS;
    const Clock::time_point submitted = Clock::now();
    bool done = false;
    for (;;) {
        done = submitAndWait(100'000'000ull, r);
        if (done || r != VK_TIMEOUT) break;
        // Still running: keep waiting on the same value.
        VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wi.semaphoreCount = 1;
        wi.pSemaphores = &timeline;
        wi.pValues = &timelineValue;
        while ((r = vk.vkWaitSemaphores(device, &wi, 100'000'000ull)) == VK_TIMEOUT) {
            if (msSince(submitted) > 10000.0) break;
        }
        done = r == VK_SUCCESS;
        break;
    }
    if (!done) {
        markLost(r == VK_ERROR_DEVICE_LOST ? "VK_ERROR_DEVICE_LOST" : std::format("secondary submission failed: {}", vkResultName(r)));
        return false;
    }
    if (ts) {
        u64 t[kQueriesPerJob]{};
        if (vk.vkGetQueryPoolResults(device, queries, 0, kQueriesPerJob, sizeof(t), t, sizeof(u64), VK_QUERY_RESULT_64_BIT) ==
            VK_SUCCESS) {
            const f64 p = props.limits.timestampPeriod * 1e-6;
            job.timing.uploadMs = f64(t[1] - t[0]) * p;
            job.timing.computeMs = f64(t[2] - t[1]) * p;
            job.timing.downloadMs = f64(t[3] - t[2]) * p;
        }
    }
    // 5. Staging: CPU copy secondary readback -> primary upload.
    if (c.path == CrossDeviceTransfer::Staging && c.downBytes > 0) {
        const Clock::time_point t = Clock::now();
        std::memcpy(c.primarySide[1].mapped + job.slot * c.downSlot, c.secondarySide[1].mapped + job.slot * c.downSlot,
                    size_t(c.downBytes));
        job.timing.cpuCopyMs += msSince(t);
    }
    job.timing.latencyMs = msSince(exported);
    return true;
}

std::unique_ptr<CrossDeviceChannel> SecondaryDevice::createChannel(const CrossDeviceChannelDesc& desc, std::string* error) {
    auto impl = std::make_unique<CrossDeviceChannel::Impl>();
    impl->dev = m_impl.get();
    impl->desc = desc;
    impl->desc.slots = std::max(desc.slots, 1u);
    impl->path = m_impl->transfer.path;
    impl->primaryFamily = m_impl->primary->queueFamily(QueueType::Graphics);
    std::string err;
    if (m_impl->lost) err = "secondary device lost";
    if (err.empty() && !impl->create(err)) impl->destroy();
    if (!err.empty()) {
        if (error) *error = err;
        return nullptr;
    }
    return std::make_unique<CrossDeviceChannel>(std::move(impl));
}

// ---------------------------------------------------------------------------------------------------------------
// CrossDeviceChannel

CrossDeviceChannel::CrossDeviceChannel(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

CrossDeviceChannel::~CrossDeviceChannel() {
    if (!m_impl) return;
    {
        std::unique_lock lock(m_impl->dev->jobMutex);
        m_impl->dev->doneCv.wait(lock, [&] { return m_impl->pending == 0; });
    }
    m_impl->destroy();
}

const CrossDeviceChannelDesc& CrossDeviceChannel::desc() const { return m_impl->desc; }
CrossDeviceTransfer CrossDeviceChannel::path() const { return m_impl->path; }
u64 CrossDeviceChannel::uploadBytes() const { return m_impl->upBytes; }
u64 CrossDeviceChannel::downloadBytes() const { return m_impl->downBytes; }

bool CrossDeviceChannel::Impl::create(std::string& error) {
    SecondaryDevice::Impl& d = *dev;
    // Texture usage by role; regions in the per-slot transfer layout.
    for (u32 i = 0; i < desc.textures.size(); ++i) {
        const bool input = std::find(desc.inputs.begin(), desc.inputs.end(), i) != desc.inputs.end();
        const bool output = std::find(desc.outputs.begin(), desc.outputs.end(), i) != desc.outputs.end();
        VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        if (input) usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (output) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (!input || output) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        SecondaryTexture t;
        if (!d.createTexture(desc.textures[i], usage, t, error)) {
            textures.push_back(t);
            return false;
        }
        textures.push_back(t);
    }
    auto layoutRegions = [&](const std::vector<u32>& list, std::vector<Region>& regions) {
        u64 offset = 0;
        for (u32 i : list) {
            const TextureDesc& td = desc.textures.at(i);
            const u64 size = mipLevelSize(td.format, td.width, td.height, 1, 0);
            regions.push_back({i, offset, size});
            offset = alignUp(offset + size, kRegionAlignment);
        }
        return offset;
    };
    const u64 upPayload = layoutRegions(desc.inputs, up);
    const u64 downPayload = layoutRegions(desc.outputs, down);
    upBytes = 0;
    for (const Region& r : up) upBytes = std::max(upBytes, r.offset + r.size);
    downBytes = 0;
    for (const Region& r : down) downBytes = std::max(downBytes, r.offset + r.size);
    const u64 slotAlign = std::max<u64>(kRegionAlignment, path == CrossDeviceTransfer::HostPointer ? d.caps.hostPointerAlignment : 0);
    upSlot = alignUp(std::max<u64>(upPayload, kRegionAlignment), slotAlign);
    downSlot = alignUp(std::max<u64>(downPayload, kRegionAlignment), slotAlign);
    slots.assign(desc.slots, Slot{});
    for (u32 dir = 0; dir < 2; ++dir) {
        const u64 size = (dir == 0 ? upSlot : downSlot) * desc.slots;
        bool ok = false;
        switch (path) {
        case CrossDeviceTransfer::ExternalMemory: ok = createSharedOpaque(dir, size, error); break;
        case CrossDeviceTransfer::HostPointer: ok = createSharedHost(dir, size, error); break;
        default: ok = createStaging(dir, size, error); break;
        }
        if (!ok) return false;
    }
    return true;
}

namespace {

VkBuffer makeBuffer(PFN_vkCreateBuffer create, VkDevice device, u64 size, VkExternalMemoryHandleTypeFlags external) {
    VkExternalMemoryBufferCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    ext.handleTypes = external;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.pNext = external ? &ext : nullptr;
    bi.size = size;
    bi.usage = kTransferBufferUsage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer b = VK_NULL_HANDLE;
    return create(device, &bi, nullptr, &b) == VK_SUCCESS ? b : VK_NULL_HANDLE;
}

} // namespace

bool CrossDeviceChannel::Impl::createSharedOpaque(u32 dir, u64 size, std::string& error) {
    SecondaryDevice::Impl& d = *dev;
    const VkDevice pdev = d.primaryDevice;
    const ExternalBufferSupport sup = externalBufferSupport(d.primary->vkPhysicalDevice(), kOpaqueHandle);
    ChannelSide& p = primarySide[dir];
    ChannelSide& s = secondarySide[dir];
    p.buffer = makeBuffer(vkCreateBuffer, pdev, size, kOpaqueHandle);
    if (!p.buffer) {
        error = "primary exportable buffer creation failed";
        return false;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(pdev, p.buffer, &req);
    const u32 type = findMemoryType(d.primaryMemProps, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    VkExportMemoryAllocateInfo exp{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    exp.handleTypes = kOpaqueHandle;
    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.buffer = p.buffer;
    if (sup.dedicatedOnly) exp.pNext = &ded;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &exp;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (type == ~0u || vkAllocateMemory(pdev, &ai, nullptr, &p.memory) != VK_SUCCESS) {
        error = "primary exportable memory allocation failed";
        return false;
    }
    vkBindBufferMemory(pdev, p.buffer, p.memory, 0);

    // Export the payload and import it on the secondary (same physical device: identical memory types).
    s.buffer = makeBuffer(d.vk.vkCreateBuffer, d.device, size, kOpaqueHandle);
    if (!s.buffer) {
        error = "secondary importable buffer creation failed";
        return false;
    }
    VkMemoryRequirements sreq{};
    d.vk.vkGetBufferMemoryRequirements(d.device, s.buffer, &sreq);
    if (!(sreq.memoryTypeBits & (1u << type)) || sreq.size > req.size) {
        error = "secondary buffer is incompatible with the exported memory";
        return false;
    }
    VkMemoryDedicatedAllocateInfo sded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    sded.buffer = s.buffer;
#ifdef _WIN32
    auto getHandle = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(vkGetDeviceProcAddr(pdev, "vkGetMemoryWin32HandleKHR"));
    if (!getHandle) {
        error = "vkGetMemoryWin32HandleKHR unavailable on the primary device";
        return false;
    }
    VkMemoryGetWin32HandleInfoKHR gi{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
    gi.memory = p.memory;
    gi.handleType = kOpaqueHandle;
    HANDLE handle = nullptr;
    if (getHandle(pdev, &gi, &handle) != VK_SUCCESS || !handle) {
        error = "vkGetMemoryWin32HandleKHR failed";
        return false;
    }
    VkImportMemoryWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
    imp.handleType = kOpaqueHandle;
    imp.handle = handle;
#else
    VkMemoryGetFdInfoKHR gi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
    gi.memory = p.memory;
    gi.handleType = kOpaqueHandle;
    int fd = -1;
    if (!vkGetMemoryFdKHR || vkGetMemoryFdKHR(pdev, &gi, &fd) != VK_SUCCESS || fd < 0) {
        error = "vkGetMemoryFdKHR failed";
        return false;
    }
    VkImportMemoryFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
    imp.handleType = kOpaqueHandle;
    imp.fd = fd; // ownership moves to the implementation on success
#endif
    if (sup.dedicatedOnly) imp.pNext = &sded;
    VkMemoryAllocateInfo sai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    sai.pNext = &imp;
    sai.allocationSize = req.size;
    sai.memoryTypeIndex = type;
    const VkResult r = d.vk.vkAllocateMemory(d.device, &sai, nullptr, &s.memory);
#ifdef _WIN32
    CloseHandle(handle); // importing an NT handle does not transfer its ownership
#else
    if (r != VK_SUCCESS) close(fd);
#endif
    if (r != VK_SUCCESS) {
        error = std::format("importing the exported memory on the secondary failed: {}", vkResultName(r));
        s.memory = VK_NULL_HANDLE;
        return false;
    }
    d.vk.vkBindBufferMemory(d.device, s.buffer, s.memory, 0);
    return true;
}

bool CrossDeviceChannel::Impl::createSharedHost(u32 dir, u64 size, std::string& error) {
    SecondaryDevice::Impl& d = *dev;
    const u64 align = std::max<u64>(d.caps.hostPointerAlignment, 4096);
    size = alignUp(size, align);
    hostMemory[dir] = hostAlloc(size, align);
    if (!hostMemory[dir]) {
        error = "host allocation failed";
        return false;
    }
    std::memset(hostMemory[dir], 0, size_t(size));
    auto import = [&](VkDevice device, const VkPhysicalDeviceMemoryProperties& mp, PFN_vkCreateBuffer createBuffer,
                      PFN_vkGetBufferMemoryRequirements getReq, PFN_vkGetMemoryHostPointerPropertiesEXT hostProps,
                      PFN_vkAllocateMemory allocate, PFN_vkBindBufferMemory bind, ChannelSide& side) {
        side.buffer = makeBuffer(createBuffer, device, size, kHostHandle);
        if (!side.buffer) return false;
        VkMemoryRequirements req{};
        getReq(device, side.buffer, &req);
        VkMemoryHostPointerPropertiesEXT hp{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        if (hostProps(device, kHostHandle, hostMemory[dir], &hp) != VK_SUCCESS) return false;
        const u32 type = findMemoryType(mp, req.memoryTypeBits & hp.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (type == ~0u) return false;
        VkImportMemoryHostPointerInfoEXT imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
        imp.handleType = kHostHandle;
        imp.pHostPointer = hostMemory[dir];
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.pNext = &imp;
        ai.allocationSize = size;
        ai.memoryTypeIndex = type;
        if (allocate(device, &ai, nullptr, &side.memory) != VK_SUCCESS) {
            side.memory = VK_NULL_HANDLE;
            return false;
        }
        bind(device, side.buffer, side.memory, 0);
        side.mapped = static_cast<u8*>(hostMemory[dir]);
        return true;
    };
    if (!import(d.primaryDevice, d.primaryMemProps, vkCreateBuffer, vkGetBufferMemoryRequirements,
                vkGetMemoryHostPointerPropertiesEXT, vkAllocateMemory, vkBindBufferMemory, primarySide[dir])) {
        error = "importing host memory on the primary failed";
        return false;
    }
    if (!import(d.device, d.memProps, d.vk.vkCreateBuffer, d.vk.vkGetBufferMemoryRequirements,
                d.vk.vkGetMemoryHostPointerPropertiesEXT, d.vk.vkAllocateMemory, d.vk.vkBindBufferMemory, secondarySide[dir])) {
        error = "importing host memory on the secondary failed";
        return false;
    }
    return true;
}

bool CrossDeviceChannel::Impl::createStaging(u32 dir, u64 size, std::string& error) {
    SecondaryDevice::Impl& d = *dev;
    // dir 0 (uplink): primary GPU writes -> CPU reads (cached), CPU writes -> secondary GPU reads.
    // dir 1 (downlink): the other way round.
    auto make = [&](VkDevice device, const VkPhysicalDeviceMemoryProperties& mp, PFN_vkCreateBuffer createBuffer,
                    PFN_vkGetBufferMemoryRequirements getReq, PFN_vkAllocateMemory allocate, PFN_vkBindBufferMemory bind,
                    PFN_vkMapMemory map, bool cpuReads, ChannelSide& side) {
        side.buffer = makeBuffer(createBuffer, device, size, 0);
        if (!side.buffer) return false;
        VkMemoryRequirements req{};
        getReq(device, side.buffer, &req);
        const u32 type = findMemoryType(mp, req.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                        cpuReads ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0,
                                        cpuReads ? 0 : VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (type == ~0u) return false;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        if (allocate(device, &ai, nullptr, &side.memory) != VK_SUCCESS) {
            side.memory = VK_NULL_HANDLE;
            return false;
        }
        bind(device, side.buffer, side.memory, 0);
        void* p = nullptr;
        if (map(device, side.memory, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) return false;
        side.mapped = static_cast<u8*>(p);
        return true;
    };
    if (!make(d.primaryDevice, d.primaryMemProps, vkCreateBuffer, vkGetBufferMemoryRequirements, vkAllocateMemory,
              vkBindBufferMemory, vkMapMemory, dir == 0, primarySide[dir]) ||
        !make(d.device, d.memProps, d.vk.vkCreateBuffer, d.vk.vkGetBufferMemoryRequirements, d.vk.vkAllocateMemory,
              d.vk.vkBindBufferMemory, d.vk.vkMapMemory, dir == 1, secondarySide[dir])) {
        error = "staging buffer allocation failed";
        return false;
    }
    return true;
}

void CrossDeviceChannel::Impl::destroy() {
    SecondaryDevice::Impl& d = *dev;
    // Secondary side first (its jobs are done), then the primary side once the primary GPU retired the frames that
    // may still copy from/to it. Imported host memory is freed after both devices released it.
    for (SecondaryTexture& t : textures) d.destroyTexture(t);
    textures.clear();
    for (ChannelSide& s : secondarySide) {
        if (s.buffer) d.vk.vkDestroyBuffer(d.device, s.buffer, nullptr);
        if (s.memory) d.vk.vkFreeMemory(d.device, s.memory, nullptr);
        s = {};
    }
    const VkDevice pdev = d.primaryDevice;
    std::array<ChannelSide, 2> prim{primarySide[0], primarySide[1]};
    std::array<void*, 2> host{hostMemory[0], hostMemory[1]};
    primarySide[0] = primarySide[1] = {};
    hostMemory[0] = hostMemory[1] = nullptr;
    d.primary->state().retire([pdev, prim, host] {
        for (const ChannelSide& p : prim) {
            if (p.buffer) vkDestroyBuffer(pdev, p.buffer, nullptr);
            if (p.memory) vkFreeMemory(pdev, p.memory, nullptr);
        }
        for (void* h : host) {
            if (h) hostFree(h);
        }
    });
}

void CrossDeviceChannel::recordExport(CommandList& cmd, u32 slot, std::span<const TextureHandle> inputs) {
    Impl& c = *m_impl;
    OX_ASSERT(inputs.size() == c.up.size(), "recordExport: {} inputs, channel '{}' has {}", inputs.size(), c.desc.name, c.up.size());
    OX_ASSERT(slot < c.slots.size(), "recordExport: slot {} out of range", slot);
    if (c.up.empty()) return;
    Device& primary = *c.dev->primary;
    const VkCommandBuffer cb = cmd.vk();
    for (usize i = 0; i < c.up.size(); ++i) {
        const Impl::Region& r = c.up[i];
        const TextureDesc& td = c.desc.textures[r.texture];
        VkBufferImageCopy bc{};
        bc.bufferOffset = slot * c.upSlot + r.offset;
        bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bc.imageExtent = {td.width, td.height, 1};
        vkCmdCopyImageToBuffer(cb, primary.vkImage(inputs[i]), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c.primarySide[0].buffer, 1, &bc);
    }
    VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    b.buffer = c.primarySide[0].buffer;
    b.offset = slot * c.upSlot;
    b.size = c.upSlot;
    b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    if (c.path == CrossDeviceTransfer::ExternalMemory) {
        b.srcQueueFamilyIndex = c.primaryFamily; // release to the other VkDevice
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    } else {
        b.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        b.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    }
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.bufferMemoryBarrierCount = 1;
    di.pBufferMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &di);
}

void CrossDeviceChannel::recordImport(CommandList& cmd, u32 slot, std::span<const TextureHandle> outputs) {
    Impl& c = *m_impl;
    OX_ASSERT(outputs.size() == c.down.size(), "recordImport: {} outputs, channel '{}' has {}", outputs.size(), c.desc.name, c.down.size());
    OX_ASSERT(slot < c.slots.size(), "recordImport: slot {} out of range", slot);
    if (c.down.empty()) return;
    Device& primary = *c.dev->primary;
    const VkCommandBuffer cb = cmd.vk();
    VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    b.buffer = c.primarySide[1].buffer;
    b.offset = slot * c.downSlot;
    b.size = c.downSlot;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    if (c.path == CrossDeviceTransfer::ExternalMemory) {
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL; // acquire from the other VkDevice
        b.dstQueueFamilyIndex = c.primaryFamily;
    } else {
        b.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        b.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    }
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.bufferMemoryBarrierCount = 1;
    di.pBufferMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &di);
    for (usize i = 0; i < c.down.size(); ++i) {
        const Impl::Region& r = c.down[i];
        const TextureDesc& td = c.desc.textures[r.texture];
        VkBufferImageCopy bc{};
        bc.bufferOffset = slot * c.downSlot + r.offset;
        bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bc.imageExtent = {td.width, td.height, 1};
        vkCmdCopyBufferToImage(cb, c.primarySide[1].buffer, primary.vkImage(outputs[i]), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    }
}

bool CrossDeviceChannel::submit(u32 slot, u64 ticket, TimelinePoint exportDone, std::vector<PortableDispatch> dispatches) {
    Impl& c = *m_impl;
    SecondaryDevice::Impl& d = *c.dev;
    OX_ASSERT(slot < c.slots.size(), "submit: slot {} out of range", slot);
    Job job;
    job.channel = &c;
    job.slot = slot;
    job.ticket = ticket;
    job.primaryTimeline = d.primary->timelineSemaphore(exportDone.queue);
    job.primaryValue = exportDone.value;
    bool ok = !d.lost;
    for (const PortableDispatch& pd : dispatches) {
        if (!ok) break;
        SecondaryDevice::Impl::PipelineEntry e;
        std::string err;
        if (!d.resolvePipeline(pd.pipeline, e, err)) {
            OX_LOG_WARN("rhi", "split: '{}' cannot run on the secondary: {}", pd.name, err);
            ok = false;
            break;
        }
        ResolvedDispatch r;
        r.pipeline = e.pipeline;
        r.push = pd.push;
        r.pushSize = pd.pushSize;
        for (const PortableDispatch::Binding& b : pd.bindings) {
            const SecondaryTexture& t = c.textures.at(b.texture);
            const u32 index = b.storage ? t.storage : t.sampled;
            if (index == kInvalidBindlessIndex || b.pushOffset + 4 > kMaxPushConstantSize) {
                OX_LOG_WARN("rhi", "split: '{}': binding of texture {} has no {} descriptor", pd.name, b.texture,
                            b.storage ? "storage" : "sampled");
                ok = false;
                break;
            }
            std::memcpy(r.push.data() + b.pushOffset, &index, 4);
        }
        for (u32 a = 0; a < 3; ++a) r.groups[a] = (std::max(pd.threads[a], 1u) + e.localSize[a] - 1) / e.localSize[a];
        job.dispatches.push_back(r);
    }
    std::lock_guard lock(d.jobMutex);
    Impl::Slot& s = c.slots[slot];
    if (s.state == Impl::Slot::State::Queued) {
        OX_LOG_ERROR("rhi", "split: channel '{}' slot {} reused while its job is still queued", c.desc.name, slot);
        return false;
    }
    s.ticket = ticket;
    s.timing = {};
    if (!ok || d.lost) {
        s.state = Impl::Slot::State::Failed;
        return false;
    }
    s.state = Impl::Slot::State::Queued;
    c.pending += 1;
    d.jobs.push_back(std::move(job));
    d.jobCv.notify_one();
    return true;
}

bool CrossDeviceChannel::wait(u32 slot, u64 ticket, u32 timeoutMs) {
    Impl& c = *m_impl;
    SecondaryDevice::Impl& d = *c.dev;
    std::unique_lock lock(d.jobMutex);
    const Impl::Slot& s = c.slots.at(slot);
    if (s.ticket != ticket) return false;
    const bool finished = d.doneCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
        return s.ticket != ticket || s.state != Impl::Slot::State::Queued;
    });
    if (!finished) {
        OX_LOG_WARN("rhi", "split: channel '{}' job {} timed out after {} ms", c.desc.name, ticket, timeoutMs);
        return false;
    }
    return s.ticket == ticket && s.state == Impl::Slot::State::Done;
}

bool CrossDeviceChannel::ready(u32 slot, u64 ticket) const {
    std::lock_guard lock(m_impl->dev->jobMutex);
    const Impl::Slot& s = m_impl->slots.at(slot);
    return s.ticket == ticket && s.state == Impl::Slot::State::Done;
}

CrossDeviceJobTiming CrossDeviceChannel::timing(u32 slot) const {
    std::lock_guard lock(m_impl->dev->jobMutex);
    return m_impl->slots.at(slot).timing;
}

} // namespace ox::rhi
