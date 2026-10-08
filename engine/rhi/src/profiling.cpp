#include "device_impl.hpp"

#include <oxwald/core/log.hpp>

#include <algorithm>
#include <cstring>

#if defined(OX_RHI_TRACY)
#include <tracy/Tracy.hpp>
#include <tracy/TracyVulkan.hpp>
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif !defined(__APPLE__)
#include <dlfcn.h>
#endif

namespace ox::rhi {

using namespace detail;

namespace detail {

u64 submitRaw(DeviceState& s, QueueType queue, std::span<const VkCommandBuffer> cmds, std::vector<VkSemaphoreSubmitInfo> waits,
              std::vector<VkSemaphoreSubmitInfo> signals);

void readTimestamps(DeviceState& s, FrameContext& f) {
    if (!f.queryPool) return;
    if (f.queryCount > 0) {
        std::vector<u64> values(f.queryCount, 0);
        const VkResult r = vkGetQueryPoolResults(s.device, f.queryPool, 0, f.queryCount, values.size() * sizeof(u64),
                                                 values.data(), sizeof(u64), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        if (r == VK_SUCCESS) {
            s.lastTimings.clear();
            u64 first = ~0ull;
            for (const TimestampScope& sc : f.scopes) {
                if (sc.endQuery != ~0u) first = std::min(first, values[sc.beginQuery]);
            }
            for (const TimestampScope& sc : f.scopes) {
                if (sc.endQuery == ~0u) continue;
                const u64 a = values[sc.beginQuery];
                const u64 b = values[sc.endQuery];
                const f64 ms = b >= a ? f64(b - a) * f64(s.caps.timestampPeriodNs) * 1e-6 : 0.0;
                const f64 start = a >= first ? f64(a - first) * f64(s.caps.timestampPeriodNs) * 1e-6 : 0.0;
                s.lastTimings.push_back({sc.name, ms, sc.depth, start, sc.queue});
            }
        }
        vkResetQueryPool(s.device, f.queryPool, 0, f.queryCount);
    }
    f.queryCount = 0;
    f.scopes.clear();
}

#if defined(OX_RHI_TRACY)
void createTracyContext(Device& device, DeviceState& s) {
    if (!s.caps.timestampQueries) return;
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = s.immediatePools[u32(QueueType::Graphics)];
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(s.device, &ai, &cb) != VK_SUCCESS) return;
    {
        std::lock_guard lock(*s.queue(QueueType::Graphics).mutex);
        TracyVkCtx ctx = TracyVkContext(s.physical, s.device, s.queue(QueueType::Graphics).queue, cb);
        s.tracyCtx = ctx;
    }
    vkFreeCommandBuffers(s.device, ai.commandPool, 1, &cb);
    const char name[] = "Graphics queue";
    static_cast<TracyVkCtx>(s.tracyCtx)->Name(name, u16(sizeof(name) - 1));
    (void)device;
}

void destroyTracyContext(DeviceState& s) {
    if (s.tracyCtx) {
        TracyVkDestroy(static_cast<TracyVkCtx>(s.tracyCtx));
        s.tracyCtx = nullptr;
    }
}

void collectTracy(Device& device, DeviceState& s) {
    if (!s.tracyCtx) {
        // Created on demand: the context allocates a 64K-entry timestamp pool, which MoltenVK can only emulate
        // (and complains about), so don't pay for it unless a profiler is actually attached.
        if (!TracyIsConnected) return;
        createTracyContext(device, s);
        if (!s.tracyCtx) return;
    }
    CommandList& cmd = device.commandList(QueueType::Graphics, "tracy.collect");
    TracyVkCollect(static_cast<TracyVkCtx>(s.tracyCtx), cmd.vk());
    device.submit(cmd);
}

struct TracyZone {
    std::optional<tracy::VkCtxScope> scope;
};

void* tracyZoneBegin(DeviceState& s, VkCommandBuffer cmd, QueueType queue, const std::string& name) {
    if (!s.tracyCtx || queue != QueueType::Graphics) return nullptr;
    auto* z = new TracyZone;
    static constexpr const char kFile[] = __FILE__;
    static constexpr const char kFunc[] = "RenderGraph";
    z->scope.emplace(static_cast<TracyVkCtx>(s.tracyCtx), u32(__LINE__), kFile, sizeof(kFile) - 1, kFunc, sizeof(kFunc) - 1,
                     name.c_str(), name.size(), cmd, true);
    return z;
}

void tracyZoneEnd(void* zone) { delete static_cast<TracyZone*>(zone); }
#else
void createTracyContext(Device&, DeviceState&) {}
void destroyTracyContext(DeviceState&) {}
void collectTracy(Device&, DeviceState&) {}
void* tracyZoneBegin(DeviceState&, VkCommandBuffer, QueueType, const std::string&) { return nullptr; }
void tracyZoneEnd(void*) {}
#endif

} // namespace detail

// ---------------------------------------------------------------------------------------------------------------
// RenderDoc in-application API (subset of renderdoc_app.h, RENDERDOC_API_1_1_2 layout).

namespace {

using pRENDERDOC_GetAPI = int (*)(int version, void** outAPIPointers);
[[maybe_unused]] constexpr int kRenderDocApi_1_1_2 = 10102;

// Function table order of RENDERDOC_API_1_1_2 (all entries are pointer-sized).
enum RenderDocFn : int {
    kGetAPIVersion = 0,
    kTriggerCapture = 15,
    kStartFrameCapture = 19,
    kIsFrameCapturing = 20,
    kEndFrameCapture = 21,
};

void* const* table(void* api) { return static_cast<void* const*>(api); }

} // namespace

bool RenderDocCapture::initialize(bool loadIfMissing) {
#if defined(__APPLE__)
    (void)loadIfMissing;
    m_reason = "RenderDoc is not available on macOS; use Xcode GPU frame capture (run with MTL_CAPTURE_ENABLED=1 "
               "and capture from Xcode, or set MVK_CONFIG_AUTO_GPU_CAPTURE_SCOPE for MoltenVK automatic captures)";
    return false;
#else
    pRENDERDOC_GetAPI getApi = nullptr;
#if defined(_WIN32)
    HMODULE mod = GetModuleHandleA("renderdoc.dll");
    if (!mod && loadIfMissing) mod = LoadLibraryA("renderdoc.dll");
    if (mod) getApi = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(mod, "RENDERDOC_GetAPI"));
#else
    void* mod = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
    if (!mod && loadIfMissing) mod = dlopen("librenderdoc.so", RTLD_NOW);
    if (mod) getApi = reinterpret_cast<pRENDERDOC_GetAPI>(dlsym(mod, "RENDERDOC_GetAPI"));
#endif
    if (!getApi) {
        m_reason = loadIfMissing ? "RenderDoc library not found" : "application was not launched from RenderDoc";
        return false;
    }
    void* api = nullptr;
    if (getApi(kRenderDocApi_1_1_2, &api) != 1 || !api) {
        m_reason = "RENDERDOC_GetAPI failed";
        return false;
    }
    m_api = api;
    m_reason.clear();
    OX_LOG_INFO("rhi", "RenderDoc in-app API attached");
    return true;
#endif
}

void RenderDocCapture::triggerCapture() {
    if (!m_api) return;
    reinterpret_cast<void (*)()>(table(m_api)[kTriggerCapture])();
}

void RenderDocCapture::startFrameCapture() {
    if (!m_api) return;
    reinterpret_cast<void (*)(void*, void*)>(table(m_api)[kStartFrameCapture])(nullptr, nullptr);
}

bool RenderDocCapture::endFrameCapture() {
    if (!m_api) return false;
    return reinterpret_cast<u32 (*)(void*, void*)>(table(m_api)[kEndFrameCapture])(nullptr, nullptr) == 1;
}

bool RenderDocCapture::isFrameCapturing() const {
    if (!m_api) return false;
    return reinterpret_cast<u32 (*)()>(table(m_api)[kIsFrameCapturing])() == 1;
}

} // namespace ox::rhi
