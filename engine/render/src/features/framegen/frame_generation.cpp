// FSR 3.1 frame generation through AMD's ffx-api (amd_fidelityfx_vk.dll, loaded at run time). See
// oxwald/render/features/framegen/frame_generation.hpp for the integration overview.
#include <oxwald/render/features/framegen/frame_generation.hpp>

#include <oxwald/core/cvar.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/render/frame_resources.hpp>
#include <oxwald/render/render_feature.hpp>
#include <oxwald/render/render_types.hpp>
#include <oxwald/render/render_view.hpp>
#include <oxwald/render/renderer.hpp>
#include <oxwald/rhi/device.hpp>
#include <oxwald/rhi/render_graph.hpp>
#include <oxwald/rhi/swapchain.hpp>
#include <oxwald/rhi/vulkan.hpp>

#include <glm/glm.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <format>
#include <mutex>
#include <thread>

#if OX_RENDER_HAS_FIDELITYFX && defined(_WIN32)
#define OX_FG_FFX 1
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_api_loader.h>
#include <ffx_api/ffx_framegeneration.h>
#include <ffx_api/vk/ffx_api_vk.h>
#else
#define OX_FG_FFX 0
#endif

namespace ox::render::framegen {

namespace {

CVar<int>& cvMode() {
    static CVar<int> cv("r.FrameGeneration", 0,
                        "Frame generation: Off, FSR3 (AMD FidelityFX frame interpolation; runtime player only, needs "
                        "amd_fidelityfx_vk.dll)",
                        CVarEnum{"Off", "FSR3"}.alias("On", 1), CVarFlags::Persist);
    return cv;
}

[[maybe_unused]] const bool kRegistered = [] {
    cvMode();
    return true;
}();

#if OX_FG_FFX

// ---- runtime library ----------------------------------------------------------------------------------------------

struct FfxRuntime {
    HMODULE module = nullptr;
    ffxFunctions fn{};
    std::string error;
    bool ok = false;
};

const FfxRuntime& runtime() {
    static const FfxRuntime rt = [] {
        FfxRuntime r;
        r.module = LoadLibraryW(L"amd_fidelityfx_vk.dll");
        if (!r.module) {
            r.error = "amd_fidelityfx_vk.dll not found next to the executable (vcpkg port amd-fidelityfx-vk)";
            return r;
        }
        ffxLoadFunctions(&r.fn, r.module);
        r.ok = r.fn.CreateContext && r.fn.DestroyContext && r.fn.Configure && r.fn.Query && r.fn.Dispatch;
        if (!r.ok) r.error = "amd_fidelityfx_vk.dll lacks the ffx-api entry points";
        return r;
    }();
    return rt;
}

const char* ffxError(ffxReturnCode_t rc) {
    switch (rc) {
    case FFX_API_RETURN_OK: return "ok";
    case FFX_API_RETURN_ERROR: return "error";
    case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE: return "unknown descriptor type";
    case FFX_API_RETURN_ERROR_RUNTIME_ERROR: return "runtime error";
    case FFX_API_RETURN_NO_PROVIDER: return "no provider";
    case FFX_API_RETURN_ERROR_MEMORY: return "out of memory";
    case FFX_API_RETURN_ERROR_PARAMETER: return "invalid parameter";
    default: return "?";
    }
}

FfxApiResource imageResource(VkImage image, VkFormat format, u32 width, u32 height, u32 state) {
    FfxApiResourceDescription d{};
    d.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    d.format = ffxApiGetSurfaceFormatVK(format);
    d.width = width;
    d.height = height;
    d.depth = 1;
    d.mipCount = 1;
    d.flags = FFX_API_RESOURCE_FLAGS_NONE;
    d.usage = FFX_API_RESOURCE_USAGE_READ_ONLY;
    if (ffxApiIsDepthFormat(format)) d.usage |= FFX_API_RESOURCE_USAGE_DEPTHTARGET;
    return ffxApiGetResourceVK(reinterpret_cast<void*>(image), d, state);
}

// Submit function of the proxy's game queue (ffx-api passes no user pointer: one generator per process).
std::atomic<VkQueue> g_gameQueue{VK_NULL_HANDLE};
std::atomic<std::mutex*> g_gameQueueMutex{nullptr};
thread_local bool t_holdsGameQueueLock = false;

VkResult submitToGameQueue(uint32_t submitCount, const VkSubmitInfo* submits, VkFence fence) {
    if (t_holdsGameQueueLock) return vkQueueSubmit(g_gameQueue.load(), submitCount, submits, fence);
    std::lock_guard lock(*g_gameQueueMutex.load());
    return vkQueueSubmit(g_gameQueue.load(), submitCount, submits, fence);
}

ffxReturnCode_t dispatchFrameGeneration(ffxDispatchDescFrameGeneration* params, void* user) {
    return runtime().fn.Dispatch(static_cast<ffxContext*>(user), &params->header);
}

VkImageLayout layoutOf(u32 state) {
    if (state & FFX_API_RESOURCE_STATE_PRESENT) return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    if (state & FFX_API_RESOURCE_STATE_COPY_DEST) return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    if (state & FFX_API_RESOURCE_STATE_COPY_SRC) return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (state & FFX_API_RESOURCE_STATE_RENDER_TARGET) return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    if (state & (FFX_API_RESOURCE_STATE_COMPUTE_READ | FFX_API_RESOURCE_STATE_PIXEL_READ)) {
        return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    return VK_IMAGE_LAYOUT_GENERAL;
}

void imageBarrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &di);
}

// ---- generator ----------------------------------------------------------------------------------------------------

class Fsr3FrameGenerator;

// Overlay, before editor overlays (order 0) and UI (10000), after LdrPost (-100000): SceneColorLDR is the HUD-less
// image here.
class FrameGenerationFeature final : public IRenderFeature {
public:
    explicit FrameGenerationFeature(Fsr3FrameGenerator& gen) : m_gen(gen) {}
    std::string_view name() const override { return "FrameGeneration"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::Overlay); }
    i32 order() const override { return -50000; }
    std::vector<std::string> cvarNames() const override { return {"r.FrameGeneration"}; }
    void setup(FeatureContext& ctx) override;

private:
    Fsr3FrameGenerator& m_gen;
};

class Fsr3FrameGenerator final : public FrameGenerator, public rhi::ISwapchainProxy {
public:
    Fsr3FrameGenerator(rhi::Device& device, Renderer& renderer, u32 mainView)
        : m_device(device), m_renderer(renderer), m_mainView(mainView) {
        m_feature = &renderer.features().add(std::make_unique<FrameGenerationFeature>(*this));
    }
    ~Fsr3FrameGenerator() override {
        destroyContexts();
        // The renderer (and the feature it owns) may already be gone in shutdown order.
    }

    // ---- FrameGenerator ----
    void beginFrame(rhi::Swapchain& swapchain, bool enable) override {
        m_wantEnabled = enable;
        if (!enable) m_createFailed = false;
        if (m_createFailed) enable = false; // the proxy could not be created: stay on the plain swapchain
        // The proxy stays installed while frame generation is merely toggled off at run time would still cost an
        // extra copy per present; leave it entirely instead (swapchain recreated on the next acquire).
        swapchain.setProxy(enable ? static_cast<rhi::ISwapchainProxy*>(this) : nullptr);
    }

    void prepareFrame(rhi::Swapchain& swapchain, f32 frameTimeMs) override {
        m_armed = false;
        if (swapchain.activeProxy() != this || !m_fgCtx) return;
        const auto t0 = std::chrono::steady_clock::now();
        ++m_frameId;
        ffxConfigureDescFrameGeneration cfg{};
        cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        cfg.swapChain = m_proxySwapchain;
        cfg.frameGenerationCallback = dispatchFrameGeneration;
        cfg.frameGenerationCallbackUserContext = &m_fgCtx;
        cfg.frameGenerationEnabled = m_wantEnabled;
        cfg.presentCallback = &Fsr3FrameGenerator::presentCallback; // composition: see presentCallback
        cfg.presentCallbackUserContext = this;
        cfg.allowAsyncWorkloads = false; // interpolation on the game queue inside present: HUD-less needs no double buffer
        cfg.HUDLessColor = imageResource(m_device.vkImage(m_hudless), m_hudlessFormat, m_extent.width, m_extent.height,
                                         FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        cfg.generationRect = {0, 0, i32(m_extent.width), i32(m_extent.height)};
        cfg.frameID = m_frameId;
        const ffxReturnCode_t rc = runtime().fn.Configure(&m_fgCtx, &cfg.header);
        if (rc != FFX_API_RETURN_OK) {
            setStatus(std::format("ffxConfigure(frame generation): {}", ffxError(rc)));
            return;
        }
        m_frameTimeMs = frameTimeMs > 0.0f ? frameTimeMs : 16.6f;
        m_armed = true;
        m_preparedThisFrame = false;
        std::lock_guard lock(m_statsMutex);
        m_stats.lastConfigureMs = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }

    void endFrame(rhi::Swapchain& swapchain) override {
        const bool active = swapchain.activeProxy() == this && m_proxySwapchain;
        std::lock_guard lock(m_statsMutex);
        m_stats.active = active && m_wantEnabled;
        if (!active) return;
        if (!m_preparedThisFrame && m_armed && m_warnedMissingPrepare++ == 0) {
            OX_LOG_WARN("render", "frame generation: the main view recorded no prepare pass this frame");
        }
        m_armed = false;
        m_stats.renderedFrames++;
        if (m_fns.pOutGetLastPresentCountFFXAPI) {
            m_stats.presentedFrames = m_fns.pOutGetLastPresentCountFFXAPI(m_proxySwapchain) - m_presentCountAtStart;
        }
    }

    bool proxyActive() const override { return m_proxySwapchain != VK_NULL_HANDLE; }

    Stats stats() const override {
        std::lock_guard lock(m_statsMutex);
        return m_stats;
    }

    void release(rhi::Swapchain& swapchain) override { swapchain.setProxy(nullptr); }

    void captureNextGeneratedFrame() override {
        std::lock_guard lock(m_captureMutex);
        if (!m_captureBuffer && m_extent.width > 0) {
            rhi::BufferDesc bd;
            bd.size = u64(m_extent.width) * m_extent.height * 4;
            bd.usage = rhi::BufferUsage::TransferDst;
            bd.memory = rhi::MemoryUsage::Readback;
            bd.name = "FrameGeneration.Capture";
            m_captureBuffer = m_device.createBuffer(bd);
        }
        if (!m_captureEvent) {
            VkEventCreateInfo ei{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
            if (vkCreateEvent(m_device.vkDevice(), &ei, nullptr, &m_captureEvent) != VK_SUCCESS) m_captureEvent = VK_NULL_HANDLE;
        }
        if (m_captureEvent) vkResetEvent(m_device.vkDevice(), m_captureEvent); // no copy is pending (see capturedFrame)
        m_capture = {};
        m_captureRecorded = false;
        m_capturePending = m_captureBuffer.valid() && m_captureEvent;
    }
    CapturedFrame capturedFrame() override {
        std::lock_guard lock(m_captureMutex);
        if (m_captureRecorded && !m_capture.done && m_captureBuffer) {
            // The copy was recorded into a present command buffer that the proxy's present thread submits on its own
            // queue; that command buffer sets m_captureEvent after the copy. Poll it with a bound instead of a blocking
            // wait: ffx's WAIT_FOR_PRESENTS (and vkDeviceWaitIdle racing the proxy's threads on its queues) could
            // block forever here while the proxy still expects the next frame.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            VkResult st = VK_EVENT_RESET;
            while ((st = vkGetEventStatus(m_device.vkDevice(), m_captureEvent)) == VK_EVENT_RESET &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (st == VK_EVENT_SET) {
                m_capture.pixels = m_device.readBuffer(m_captureBuffer, 0, u64(m_capture.width) * m_capture.height * 4);
                m_capture.done = true;
            } else {
                OX_LOG_WARN("render", "frame generation: the captured frame's copy did not complete within 2 s ({})",
                            st == VK_EVENT_RESET ? "not executed yet" : "device error");
            }
        }
        return m_capture;
    }
    u64 currentFrameId() const override { return m_frameId; }

    // ---- rhi::ISwapchainProxy (render thread, inside Swapchain::build/acquire/present) ----
    VkResult createSwapchain(const VkSwapchainCreateInfoKHR& info, VkSwapchainKHR& out) override {
        destroyContexts();
        const FfxRuntime& rt = runtime();
        const rhi::FrameGenerationQueues q = m_device.frameGenerationQueues();
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        ffxCreateContextDescFrameGenerationSwapChainVK sc{};
        sc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
        sc.physicalDevice = m_device.vkPhysicalDevice();
        sc.device = m_device.vkDevice();
        sc.swapchain = &swapchain;
        sc.createInfo = info;
        sc.createInfo.pNext = nullptr;
        sc.createInfo.oldSwapchain = VK_NULL_HANDLE;
        // The game queue is the engine's graphics queue, which engine threads submit to under its mutex. The proxy
        // submits to it from our present call (we hold the mutex there, see present()) and, in its compose-on-game-
        // queue mode (a present queue without graphics/compute; not ours, the private present queue is in the
        // graphics family), from its present thread — which must then take the mutex. The submit function covers
        // both (an unsynchronised vkQueueSubmit on one queue is undefined behaviour).
        g_gameQueue.store(m_device.vkQueue(rhi::QueueType::Graphics));
        g_gameQueueMutex.store(&m_device.graphicsQueueMutex());
        sc.gameQueue = {m_device.vkQueue(rhi::QueueType::Graphics), m_device.queueFamily(rhi::QueueType::Graphics),
                        &submitToGameQueue};
        sc.asyncComputeQueue = {q.asyncCompute.queue, q.asyncCompute.family, nullptr};
        sc.presentQueue = {q.present.queue, q.present.family, nullptr};
        sc.imageAcquireQueue = {q.imageAcquire.queue, q.imageAcquire.family, nullptr};
        const auto t0 = std::chrono::steady_clock::now();
        ffxReturnCode_t rc = rt.fn.CreateContext(&m_scCtx, &sc.header, nullptr);
        if (rc != FFX_API_RETURN_OK || !swapchain) {
            setStatus(std::format("ffxCreateContext(FG swapchain): {}", ffxError(rc)));
            m_scCtx = nullptr;
            m_createFailed = true; // not retried every frame; toggling r.FrameGeneration off and on retries
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        m_fns = {};
        m_fns.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;
        rc = rt.fn.Query(&m_scCtx, &m_fns.header);
        if (rc != FFX_API_RETURN_OK || !m_fns.pOutAcquireNextImageKHR || !m_fns.pOutQueuePresentKHR) {
            setStatus(std::format("ffxQuery(swapchain functions): {}", ffxError(rc)));
            rt.fn.DestroyContext(&m_scCtx, nullptr);
            m_scCtx = nullptr;
            m_createFailed = true; // not retried every frame; toggling r.FrameGeneration off and on retries
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        m_proxySwapchain = swapchain;
        m_extent = info.imageExtent;
        // HUD-less copy of SceneColorLDR (display encoded RGBA8): typed sRGB when the back buffer is sRGB so FSR
        // decodes both the same way (the bytes are copied unchanged).
        const bool srgb = info.imageFormat == VK_FORMAT_B8G8R8A8_SRGB || info.imageFormat == VK_FORMAT_R8G8B8A8_SRGB;
        m_hudlessFormat = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        rhi::TextureDesc td;
        td.name = "FrameGeneration.HudLess";
        td.format = m_hudlessFormat;
        td.width = m_extent.width;
        td.height = m_extent.height;
        td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
        m_hudless = m_device.createTexture(td);
        // Scratch image for encoding generated frames (see presentCallback): the UNORM twin of an sRGB back buffer.
        const VkFormat unormTwin = info.imageFormat == VK_FORMAT_B8G8R8A8_SRGB   ? VK_FORMAT_B8G8R8A8_UNORM
                                   : info.imageFormat == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM
                                                                                  : VK_FORMAT_UNDEFINED;
        if (unormTwin != VK_FORMAT_UNDEFINED) {
            VkFormatProperties srcProps{}, dstProps{};
            vkGetPhysicalDeviceFormatProperties(m_device.vkPhysicalDevice(), unormTwin, &srcProps);
            vkGetPhysicalDeviceFormatProperties(m_device.vkPhysicalDevice(), info.imageFormat, &dstProps);
            if ((srcProps.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
                (dstProps.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
                rhi::TextureDesc sd;
                sd.name = "FrameGeneration.EncodeScratch";
                sd.format = unormTwin;
                sd.width = m_extent.width;
                sd.height = m_extent.height;
                sd.usage = rhi::TextureUsage::TransferSrc | rhi::TextureUsage::TransferDst;
                m_encodeScratch = m_device.createTexture(sd);
            } else {
                OX_LOG_WARN("render", "frame generation: no blit into the sRGB back buffer; generated frames will look dark");
            }
        }

        ffxCreateBackendVKDesc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        backend.vkDevice = m_device.vkDevice();
        backend.vkPhysicalDevice = m_device.vkPhysicalDevice();
        backend.vkDeviceProcAddr = vkGetDeviceProcAddr;
        ffxCreateContextDescFrameGenerationHudless hud{};
        hud.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
        hud.header.pNext = &backend.header;
        hud.hudlessBackBufferFormat = ffxApiGetSurfaceFormatVK(m_hudlessFormat);
        ffxCreateContextDescFrameGeneration fg{};
        fg.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        fg.header.pNext = &hud.header;
        // Reversed-Z depth; the runtime camera's far plane is finite by default (CameraParams::farPlane > 0).
        fg.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
        if (m_infiniteDepth) fg.flags |= FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE;
        fg.displaySize = {m_extent.width, m_extent.height};
        fg.maxRenderSize = {m_extent.width, m_extent.height};
        fg.backBufferFormat = ffxApiGetSurfaceFormatVK(info.imageFormat);
        rc = rt.fn.CreateContext(&m_fgCtx, &fg.header, nullptr);
        if (rc != FFX_API_RETURN_OK) {
            setStatus(std::format("ffxCreateContext(frame generation): {}", ffxError(rc)));
            m_fgCtx = nullptr;
            destroyContexts();
            m_createFailed = true; // not retried every frame; toggling r.FrameGeneration off and on retries
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        m_presentCountAtStart = m_fns.pOutGetLastPresentCountFFXAPI ? m_fns.pOutGetLastPresentCountFFXAPI(m_proxySwapchain) : 0;
        {
            std::lock_guard lock(m_statsMutex);
            m_stats.renderedFrames = 0;
            m_stats.addedLatencyMs = 0.0;
            m_presentCalls = {};
            m_stats.presentedFrames = 0;
            m_stats.generatedPresented = 0;
            m_stats.realPresented = 0;
            m_stats.pacingTimeouts = 0;
            m_stats.pacingWaitMs = 0.0;
            m_lastComposedFrameId = 0;
        }
        m_frameId = 0;
        setStatus(std::format("FSR 3 frame generation: {}x{}, contexts created in {:.0f} ms", m_extent.width,
                              m_extent.height,
                              std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count()));
        out = swapchain;
        return VK_SUCCESS;
    }

    void destroySwapchain(VkSwapchainKHR swapchain) override {
        if (swapchain == m_proxySwapchain) destroyContexts();
    }
    VkResult getImages(VkSwapchainKHR swapchain, u32& count, VkImage* images) override {
        return m_fns.pOutGetSwapchainImagesKHR(m_device.vkDevice(), swapchain, &count, images);
    }
    VkResult acquire(VkSwapchainKHR swapchain, u64 timeout, VkSemaphore semaphore, u32& imageIndex) override {
        return m_fns.pOutAcquireNextImageKHR(m_device.vkDevice(), swapchain, timeout, semaphore, VK_NULL_HANDLE, &imageIndex);
    }
    VkResult present(VkQueue queue, const VkPresentInfoKHR& info) override {
        {
            std::unique_lock lock(m_statsMutex);
            m_presentCalls[m_frameId % m_presentCalls.size()] = {m_frameId, std::chrono::steady_clock::now()};
            // Hand frame N to the proxy only once its present thread picked up frame N - 1 (composed one of its frames).
            // Otherwise the proxy's interpolation thread may overwrite a scheduled pair its present thread has not taken
            // yet (FidelityFX SDK 1.1.4, interpolationThread / composeAndPresent_presenterThread: "we might have dropped
            // entries"), and with frames dropped that way the proxy stalled for good: its present thread inside the
            // driver's vkQueuePresentKHR, its interpolation thread and our render thread (in the proxy's
            // vkQueuePresentKHR) waiting on its semaphores, our own GPU work complete. Without this wait the player hung
            // within seconds in 11 of 12 mailbox runs (Showcase stations 1 and 4, 720p/1440p, High/Ultra); with it in
            // 0 of 12 (docs/dev/modules/render_postprocess.md "Frame generation"). Bounded, so a present thread that
            // composes nothing (window minimised, no image acquired) costs the timeout, counted in
            // Stats::pacingTimeouts; the average wait is Stats::pacingWaitMs.
            if (m_frameId > 1 && m_lastComposedFrameId + 1 < m_frameId) {
                const auto t0 = std::chrono::steady_clock::now();
                if (!m_composedCv.wait_for(lock, std::chrono::milliseconds(250),
                                           [&] { return m_lastComposedFrameId + 1 >= m_frameId; })) {
                    ++m_stats.pacingTimeouts;
                }
                m_stats.pacingWaitMs += (std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count() -
                                         m_stats.pacingWaitMs) / 16.0;
            } else {
                m_stats.pacingWaitMs -= m_stats.pacingWaitMs / 16.0;
            }
        }
        // rhi::Swapchain::present holds the graphics queue mutex around this call: submissions the proxy makes from
        // inside it must not take it again (submitToGameQueue).
        t_holdsGameQueueLock = true;
        const VkResult r = m_fns.pOutQueuePresentKHR(queue, &info);
        t_holdsGameQueueLock = false;
        return r;
    }

    // ---- feature side (render thread, graph declaration) ----
    void declarePasses(FeatureContext& ctx) {
        if (!m_armed || m_preparedThisFrame || ctx.view().id() != m_mainView || !m_hudless) return;
        FrameResources& R = ctx.resources();
        const rhi::RGTexture depth = R.texture(res::kDepth);
        const rhi::RGTexture velocity = R.texture(res::kVelocity);
        const rhi::RGTexture ldr = R.texture(res::kSceneColorLDR);
        if (!depth.valid() || !velocity.valid() || !ldr.valid()) return;
        const Extent2D out = ctx.outputExtent();
        if (out.width != m_extent.width || out.height != m_extent.height) return; // resize in flight
        m_preparedThisFrame = true;
        rhi::Device& dev = ctx.device();
        const rhi::RGTexture hudless = ctx.graph().importTexture(dev, m_hudless, rhi::Access::SampledCompute);
        ctx.graph()
            .addPass("FrameGeneration.HudLess", rhi::PassType::Transfer)
            .read(ldr, rhi::Access::TransferRead)
            .overwrite(hudless, rhi::Access::TransferWrite)
            .execute([ldr, hudless](rhi::PassContext& p) { p.cmd.copyTexture(p.texture(ldr), p.texture(hudless)); });

        const RenderView& view = ctx.view();
        const CameraParams& cam = view.camera();
        const Extent2D in = ctx.renderExtent();
        ffxDispatchDescFrameGenerationPrepare prep{};
        prep.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
        prep.frameID = m_frameId;
        prep.renderSize = {in.width, in.height};
        prep.jitterOffset = {view.jitterPixels().x, view.jitterPixels().y};
        // Velocity is uvCurrent - uvPrevious (unjittered, y down); FSR wants current -> previous in pixels.
        prep.motionVectorScale = {-f32(in.width), -f32(in.height)};
        prep.frameTimeDelta = m_frameTimeMs;
        prep.cameraNear = cam.nearPlane;
        prep.cameraFar = cam.farPlane > cam.nearPlane ? cam.farPlane : 1.0e6f;
        prep.cameraFovAngleVertical = cam.verticalFov;
        prep.viewSpaceToMetersFactor = 1.0f;
        ffxDispatchDescFrameGenerationPrepareCameraInfo camInfo{};
        camInfo.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO;
        const glm::vec3 pos = cam.position();
        const glm::vec3 right = glm::normalize(glm::vec3(cam.world[0]));
        const glm::vec3 up = glm::normalize(glm::vec3(cam.world[1]));
        const glm::vec3 forward = -glm::normalize(glm::vec3(cam.world[2]));
        for (int i = 0; i < 3; ++i) {
            camInfo.cameraPosition[i] = pos[i];
            camInfo.cameraUp[i] = up[i];
            camInfo.cameraRight[i] = right[i];
            camInfo.cameraForward[i] = forward[i];
        }
        const bool infinite = cam.farPlane <= 0.0f;
        if (infinite != m_infiniteDepth) m_infiniteDepth = infinite; // applied with the next context (resize/toggle)
        ffxContext* fgCtx = &m_fgCtx;
        rhi::Device* device = &dev;
        ctx.graph()
            .addPass("FrameGeneration.Prepare", rhi::PassType::Compute)
            .read(depth, rhi::Access::SampledCompute)
            .read(velocity, rhi::Access::SampledCompute)
            .sideEffect()
            .execute([=](rhi::PassContext& p) mutable {
                const rhi::TextureHandle d = p.texture(depth), v = p.texture(velocity);
                const rhi::TextureDesc& dd = device->desc(d);
                const rhi::TextureDesc& vd = device->desc(v);
                prep.commandList = p.cmd.vk();
                prep.depth = imageResource(device->vkImage(d), dd.format, dd.width, dd.height,
                                           FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
                prep.motionVectors = imageResource(device->vkImage(v), vd.format, vd.width, vd.height,
                                                   FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
                prep.header.pNext = &camInfo.header;
                const ffxReturnCode_t rc = runtime().fn.Dispatch(fgCtx, &prep.header);
                if (rc != FFX_API_RETURN_OK) OX_LOG_WARN("render", "ffxDispatch(frame generation prepare): {}", ffxError(rc));
            });
    }

private:
    // FSR present thread, every presented frame: composes the rendered or generated back buffer into the swapchain
    // image and grabs the first generated frame while a capture is pending.
    // Always our callback, never the proxy's built-in composition: with an sRGB back buffer (our default swapchain) the
    // interpolated image holds *linear* values in its sRGB-typed bytes (FSR writes it through a UNORM alias without
    // encoding; measured: generated bytes == sRGB-decode(expected bytes) on every pixel). A raw copy shows generated
    // frames far too dark (flicker at 2x). Generated frames therefore go copy -> UNORM scratch image (raw bytes, now
    // read as the linear values they are) -> blit into the sRGB swapchain image (the blit encodes). Rendered frames and
    // UNORM back buffers (FSR encodes those itself) are copied unchanged.
    static ffxReturnCode_t presentCallback(ffxCallbackDescFrameGenerationPresent* p, void* user) {
        auto* self = static_cast<Fsr3FrameGenerator*>(user);
        VkCommandBuffer cb = static_cast<VkCommandBuffer>(p->commandList);
        VkImage src = static_cast<VkImage>(p->currentBackBuffer.resource);
        VkImage dst = static_cast<VkImage>(p->outputSwapChainBuffer.resource);
        const VkImageLayout srcLayout = layoutOf(p->currentBackBuffer.state);
        const VkImageLayout dstLayout = layoutOf(p->outputSwapChainBuffer.state);
        const u32 w = p->currentBackBuffer.description.width, h = p->currentBackBuffer.description.height;
        imageBarrier(cb, src, srcLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        imageBarrier(cb, dst, dstLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy c{};
        c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.extent = {w, h, 1};
        const VkImage scratch = self->m_encodeScratch ? self->m_device.vkImage(self->m_encodeScratch) : VK_NULL_HANDLE;
        const bool encode = p->isGeneratedFrame && scratch && w == self->m_extent.width && h == self->m_extent.height;
        if (encode) {
            imageBarrier(cb, scratch, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdCopyImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scratch, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            imageBarrier(cb, scratch, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkImageBlit blit{};
            blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[1] = blit.dstOffsets[1] = {i32(w), i32(h), 1};
            vkCmdBlitImage(cb, scratch, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                           VK_FILTER_NEAREST);
        } else {
            vkCmdCopyImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        }
        VkImageLayout dstNow = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        self->recordPresent(p->frameID, p->isGeneratedFrame);
        {
            std::lock_guard lock(self->m_captureMutex);
            if (p->isGeneratedFrame && self->m_capturePending && self->m_captureBuffer && self->m_captureEvent) {
                // The composed swapchain image: exactly what is displayed.
                imageBarrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                dstNow = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                VkBufferImageCopy bc{};
                bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                bc.imageExtent = {w, h, 1};
                vkCmdCopyImageToBuffer(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       self->m_device.vkBuffer(self->m_captureBuffer), 1, &bc);
                // Copy -> host visible, then flag it for capturedFrame() (polled, no blocking wait).
                VkBufferMemoryBarrier2 hb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                hb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                hb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                hb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
                hb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
                hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                hb.buffer = self->m_device.vkBuffer(self->m_captureBuffer);
                hb.size = VK_WHOLE_SIZE;
                VkDependencyInfo hdi{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                hdi.bufferMemoryBarrierCount = 1;
                hdi.pBufferMemoryBarriers = &hb;
                vkCmdPipelineBarrier2(cb, &hdi);
                vkCmdSetEvent(cb, self->m_captureEvent, VK_PIPELINE_STAGE_TRANSFER_BIT);
                self->m_capturePending = false;
                self->m_captureRecorded = true;
                self->m_capture.frameId = p->frameID;
                self->m_capture.width = w;
                self->m_capture.height = h;
            }
        }
        imageBarrier(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, srcLayout);
        imageBarrier(cb, dst, dstNow, dstLayout);
        return FFX_API_RETURN_OK;
    }

    // Present thread, a frame being composed. For a rendered frame: how long it was held since the engine presented it
    // (the generated frame before it is presented first) — the latency frame generation adds on top of rendering.
    void recordPresent(u64 frameId, bool generated) {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard lock(m_statsMutex);
        if (frameId > m_lastComposedFrameId) {
            m_lastComposedFrameId = frameId;
            m_composedCv.notify_all();
        }
        if (generated) {
            ++m_stats.generatedPresented;
            return;
        }
        ++m_stats.realPresented;
        const PresentCall& c = m_presentCalls[frameId % m_presentCalls.size()];
        if (c.frameId != frameId || c.time == std::chrono::steady_clock::time_point{}) return;
        const f64 ms = std::chrono::duration<f64, std::milli>(now - c.time).count();
        m_stats.addedLatencyMs = m_stats.addedLatencyMs > 0.0 ? m_stats.addedLatencyMs + (ms - m_stats.addedLatencyMs) / 16.0 : ms;
    }

    void destroyContexts() {
        if (!m_scCtx && !m_fgCtx && !m_hudless) return;
        m_device.waitIdle();
        const FfxRuntime& rt = runtime();
        if (m_scCtx) {
            ffxDispatchDescFrameGenerationSwapChainWaitForPresentsVK wait{};
            wait.header.type = FFX_API_DISPATCH_DESC_TYPE_FGSWAPCHAIN_WAIT_FOR_PRESENTS_VK;
            rt.fn.Dispatch(&m_scCtx, &wait.header);
        }
        if (m_fgCtx) rt.fn.DestroyContext(&m_fgCtx, nullptr);
        if (m_scCtx) rt.fn.DestroyContext(&m_scCtx, nullptr); // destroys the proxy swapchain
        m_fgCtx = nullptr;
        m_scCtx = nullptr;
        m_proxySwapchain = VK_NULL_HANDLE;
        m_fns = {};
        if (m_hudless) m_device.destroy(m_hudless);
        m_hudless = {};
        if (m_encodeScratch) m_device.destroy(m_encodeScratch);
        m_encodeScratch = {};
        {
            std::lock_guard lock(m_captureMutex);
            if (m_captureBuffer) m_device.destroy(m_captureBuffer);
            m_captureBuffer = {};
            if (m_captureEvent) vkDestroyEvent(m_device.vkDevice(), m_captureEvent, nullptr);
            m_captureEvent = VK_NULL_HANDLE;
            m_capturePending = false;
            m_captureRecorded = false;
        }
        m_armed = false;
        std::lock_guard lock(m_statsMutex);
        m_stats.active = false;
    }

    void setStatus(std::string s) {
        OX_LOG_INFO("render", "{}", s);
        std::lock_guard lock(m_statsMutex);
        m_stats.status = std::move(s);
    }

    rhi::Device& m_device;
    Renderer& m_renderer;
    u32 m_mainView;
    IRenderFeature* m_feature = nullptr;
    ffxContext m_scCtx = nullptr;
    ffxContext m_fgCtx = nullptr;
    ffxQueryDescSwapchainReplacementFunctionsVK m_fns{};
    VkSwapchainKHR m_proxySwapchain = VK_NULL_HANDLE;
    VkExtent2D m_extent{};
    rhi::TextureHandle m_hudless;
    rhi::TextureHandle m_encodeScratch; // UNORM twin of an sRGB back buffer (presentCallback)
    VkFormat m_hudlessFormat = VK_FORMAT_R8G8B8A8_UNORM;
    bool m_wantEnabled = false;
    bool m_createFailed = false;
    bool m_infiniteDepth = false;
    bool m_armed = false;
    bool m_preparedThisFrame = false;
    u32 m_warnedMissingPrepare = 0;
    u64 m_frameId = 0;
    u64 m_presentCountAtStart = 0;
    f32 m_frameTimeMs = 16.6f;
    mutable std::mutex m_statsMutex;
    Stats m_stats;
    struct PresentCall {
        u64 frameId = ~0ull;
        std::chrono::steady_clock::time_point time{};
    };
    std::array<PresentCall, 16> m_presentCalls{}; // guarded by m_statsMutex
    u64 m_lastComposedFrameId = 0;                // guarded by m_statsMutex; present thread
    std::condition_variable m_composedCv;         // m_lastComposedFrameId advanced

    std::mutex m_captureMutex;
    rhi::BufferHandle m_captureBuffer;
    VkEvent m_captureEvent = VK_NULL_HANDLE;
    bool m_capturePending = false;
    bool m_captureRecorded = false; // the copy is recorded into a present (pixels valid once m_captureEvent is set)
    CapturedFrame m_capture;
};


void FrameGenerationFeature::setup(FeatureContext& ctx) { m_gen.declarePasses(ctx); }

#endif // OX_FG_FFX

} // namespace

i32 requestedMode() { return cvMode().get(); }
void registerCVars() { cvMode(); }

void appendVulkanRequirements(rhi::DeviceDesc& desc) {
#if OX_FG_FFX
    if (!desc.surface || !runtime().ok) return;
    desc.frameGenerationQueues = true;
    // The FidelityFX Vulkan backend loads the KHR aliases of these (promoted) functions through vkGetDeviceProcAddr,
    // which returns them only when the extension is enabled; without them context creation calls a null pointer.
    desc.optionalDeviceExtensions.emplace_back(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
    desc.optionalDeviceExtensions.emplace_back(VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME);
#else
    (void)desc;
#endif
}

Availability availability(const rhi::Device& device) {
#if OX_FG_FFX
    if (!runtime().ok) return {false, runtime().error};
    if (!device.surface()) return {false, "frame generation needs a window (headless device)"};
    if (!device.frameGenerationQueues().valid()) {
        return {false, "the device was created without the frame generation queues (DeviceDesc::frameGenerationQueues)"};
    }
    if (!device.hasDeviceExtension(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME)) {
        return {false, "VK_KHR_get_memory_requirements2 is not enabled"};
    }
    return {true, {}};
#else
    (void)device;
#if defined(_WIN32)
    return {false, "built without the AMD FidelityFX SDK (vcpkg port amd-fidelityfx-vk)"};
#else
    return {false, "AMD FSR 3 frame generation for Vulkan ships as a Windows DLL only (FidelityFX SDK 1.1.4)"};
#endif
#endif
}

std::unique_ptr<FrameGenerator> FrameGenerator::create(rhi::Device& device, Renderer& renderer, u32 mainView,
                                                       std::string* reason) {
    const Availability a = availability(device);
    if (!a.available) {
        if (reason) *reason = a.reason;
        return nullptr;
    }
#if OX_FG_FFX
    return std::make_unique<Fsr3FrameGenerator>(device, renderer, mainView);
#else
    (void)renderer;
    (void)mainView;
    return nullptr;
#endif
}

} // namespace ox::render::framegen
