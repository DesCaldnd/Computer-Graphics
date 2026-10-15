#include "device_impl.hpp"

#include <oxwald/core/assert.hpp>
#include <oxwald/core/log.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <string>

namespace ox::rhi {

using namespace detail;

std::unique_ptr<Swapchain> Swapchain::create(Device& device, const SwapchainDesc& desc) {
    if (!device.surface()) {
        OX_LOG_ERROR("rhi", "Swapchain::create: the device was created headless (DeviceDesc::surface == nullptr)");
        return nullptr;
    }
    std::unique_ptr<Swapchain> sc(new Swapchain(device, desc));
    if (!sc->build()) {
        return nullptr;
    }
    return sc;
}

Swapchain::Swapchain(Device& device, const SwapchainDesc& desc) : m_device(device), m_desc(desc) {
    if (ISurfaceProvider* provider = device.surfaceProvider()) m_surfaceGeneration = provider->surfaceGeneration();
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    m_acquireSemaphores.resize(device.framesInFlight());
    for (auto& s : m_acquireSemaphores) {
        OX_VK_CHECK(vkCreateSemaphore(device.vkDevice(), &si, nullptr, &s));
    }
    m_latencyCapable = desc.lowLatency && device.caps().lowLatency2 && vkLatencySleepNV && vkSetLatencySleepModeNV &&
                       vkSetLatencyMarkerNV && vkGetLatencyTimingsNV;
    if (m_latencyCapable) {
        VkSemaphoreTypeCreateInfo ti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo tsi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        tsi.pNext = &ti;
        OX_VK_CHECK(vkCreateSemaphore(device.vkDevice(), &tsi, nullptr, &m_latencySemaphore));
        device.setDebugName(VK_OBJECT_TYPE_SEMAPHORE, u64(m_latencySemaphore), "latencySleep");
    }
}

Swapchain::~Swapchain() {
    m_device.waitIdle();
    destroyImages();
    for (VkSemaphore s : m_acquireSemaphores) vkDestroySemaphore(m_device.vkDevice(), s, nullptr);
    std::lock_guard lock(m_latencyMutex);
    if (m_latencySemaphore) vkDestroySemaphore(m_device.vkDevice(), m_latencySemaphore, nullptr);
    destroySwapchainLocked();
}

void Swapchain::destroySwapchainLocked() {
    if (m_swapchain) {
        if (m_owner) m_owner->destroySwapchain(m_swapchain);
        else vkDestroySwapchainKHR(m_device.vkDevice(), m_swapchain, nullptr);
    }
    m_swapchain = VK_NULL_HANDLE;
    m_owner = nullptr;
    m_ownerIsProxy.store(false);
}

void Swapchain::setProxy(ISwapchainProxy* proxy) {
    if (proxy != m_wantProxy) {
        m_wantProxy = proxy;
        m_dirty = true;
    }
}

void Swapchain::destroyImages() {
    for (TextureHandle t : m_textures) m_device.destroy(t);
    m_textures.clear();
    for (VkSemaphore s : m_presentSemaphores) vkDestroySemaphore(m_device.vkDevice(), s, nullptr);
    m_presentSemaphores.clear();
    m_device.waitIdle(); // run deferred view destruction before the images go away with the swapchain
}

namespace {

VkSurfaceFormatKHR chooseFormat(const std::vector<VkSurfaceFormatKHR>& formats, SwapchainColor color) {
    auto find = [&](VkFormat f, VkColorSpaceKHR cs) -> std::optional<VkSurfaceFormatKHR> {
        for (const auto& sf : formats) {
            if (sf.format == f && sf.colorSpace == cs) return sf;
        }
        return std::nullopt;
    };
    if (color == SwapchainColor::Hdr10) {
        if (auto f = find(VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT)) return *f;
        if (auto f = find(VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT)) return *f;
        OX_LOG_WARN("rhi", "HDR10 swapchain not supported by the surface; using sRGB");
    }
    if (color == SwapchainColor::ScRgb) {
        if (auto f = find(VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT)) return *f;
        OX_LOG_WARN("rhi", "scRGB swapchain not supported by the surface; using sRGB");
    }
    if (color == SwapchainColor::Unorm) {
        if (auto f = find(VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)) return *f;
        if (auto f = find(VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)) return *f;
    }
    if (auto f = find(VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)) return *f;
    if (auto f = find(VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)) return *f;
    return formats.front();
}

VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>& modes, PresentMode want, PresentMode& chosen) {
    auto has = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
    if (want == PresentMode::Immediate && has(VK_PRESENT_MODE_IMMEDIATE_KHR)) {
        chosen = PresentMode::Immediate;
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    if ((want == PresentMode::Immediate || want == PresentMode::Mailbox) && has(VK_PRESENT_MODE_MAILBOX_KHR)) {
        chosen = PresentMode::Mailbox;
        return VK_PRESENT_MODE_MAILBOX_KHR;
    }
    chosen = PresentMode::VSync;
    return VK_PRESENT_MODE_FIFO_KHR;
}

} // namespace

bool Swapchain::build() {
    DeviceState& s = m_device.state();
    if (!s.surface) { // native window lost (mobile pause): nothing to build until a new surface exists
        m_extent = {};
        return true;
    }
    VkSurfaceCapabilitiesKHR caps{};
    if (VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s.physical, s.surface, &caps); r != VK_SUCCESS) {
        OX_LOG_WARN("rhi", "vkGetPhysicalDeviceSurfaceCapabilitiesKHR: {}", vkResultName(r));
        m_extent = {};
        return r == VK_ERROR_SURFACE_LOST_KHR; // retried once the platform reports a new surface
    }
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == ~0u) {
        extent = s.surfaceProvider->framebufferSize();
        extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
        m_extent = extent;
        return true; // minimized: keep the old swapchain, retry later
    }

    u32 count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(s.physical, s.surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(s.physical, s.surface, &count, formats.data());
    vkGetPhysicalDeviceSurfacePresentModesKHR(s.physical, s.surface, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(s.physical, s.surface, &count, modes.data());
    if (formats.empty()) {
        OX_LOG_ERROR("rhi", "surface reports no formats");
        return false;
    }
    const VkSurfaceFormatKHR format = chooseFormat(formats, m_desc.color);
    const VkPresentModeKHR presentMode = choosePresentMode(modes, m_desc.presentMode, m_activeMode);

    u32 imageCount = std::max(m_desc.imageCount, caps.minImageCount);
    if (caps.maxImageCount > 0) imageCount = std::min(imageCount, caps.maxImageCount);

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = s.surface;
    ci.minImageCount = imageCount;
    ci.imageFormat = format.format;
    ci.imageColorSpace = format.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (any(m_desc.extraUsage & TextureUsage::Storage)) ci.imageUsage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (any(m_desc.extraUsage & TextureUsage::Sampled)) ci.imageUsage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                                                                        : caps.currentTransform;
    // Rendering stays upright and the compositor rotates (no pre-rotation of the projection): on Android a
    // rotated display then reports VK_SUBOPTIMAL_KHR on every present, which must not trigger a recreate loop.
    m_currentTransform = caps.currentTransform;
    m_preTransform = ci.preTransform;
    m_transformMismatch = ci.preTransform != caps.currentTransform;
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                            : VkCompositeAlphaFlagBitsKHR(caps.supportedCompositeAlpha & (~caps.supportedCompositeAlpha + 1u));
    ci.presentMode = presentMode;
    ci.clipped = VK_TRUE;
    VkSwapchainLatencyCreateInfoNV latencyInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV};
    latencyInfo.latencyModeEnable = VK_TRUE;

    // Other threads may be inside a latency call on the old handle (Reflex sleep / markers on the game thread).
    std::unique_lock latencyLock(m_latencyMutex);
    VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
    if (m_wantProxy) {
        // The proxy (FSR 3 frame generation) owns its swapchain; it cannot retire ours or a previous one of its own
        // in place: tear the old one down first.
        if (m_swapchain) {
            destroyImages();
            destroySwapchainLocked();
        }
        if (VkResult r = m_wantProxy->createSwapchain(ci, newSwapchain); r != VK_SUCCESS) {
            OX_LOG_ERROR("rhi", "swapchain proxy: create failed ({}); falling back to the plain swapchain", vkResultName(r));
            m_wantProxy = nullptr;
            latencyLock.unlock();
            return build();
        }
        m_owner = m_wantProxy;
        m_ownerIsProxy.store(true);
    } else {
        if (m_swapchain && m_owner) { // leaving the proxy
            destroyImages();
            destroySwapchainLocked();
        }
        ci.oldSwapchain = m_swapchain;
        if (m_latencyCapable) ci.pNext = &latencyInfo;
        if (VkResult r = vkCreateSwapchainKHR(s.device, &ci, nullptr, &newSwapchain); r != VK_SUCCESS) {
            OX_LOG_ERROR("rhi", "vkCreateSwapchainKHR: {}", vkResultName(r));
            return false;
        }
        if (m_swapchain) {
            destroyImages();
            vkDestroySwapchainKHR(s.device, m_swapchain, nullptr);
        }
        m_owner = nullptr;
        m_ownerIsProxy.store(false);
    }
    m_swapchain = newSwapchain;
    applyLatencyModeLocked();
    latencyLock.unlock();
    m_format = format.format;
    m_colorSpace = format.colorSpace;
    m_extent = extent;

    std::vector<VkImage> images;
    if (m_owner) {
        m_owner->getImages(m_swapchain, count, nullptr);
        images.resize(count);
        m_owner->getImages(m_swapchain, count, images.data());
    } else {
        vkGetSwapchainImagesKHR(s.device, m_swapchain, &count, nullptr);
        images.resize(count);
        vkGetSwapchainImagesKHR(s.device, m_swapchain, &count, images.data());
    }
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (u32 i = 0; i < count; ++i) {
        TextureDesc td;
        td.format = m_format;
        td.width = extent.width;
        td.height = extent.height;
        td.usage = TextureUsage::ColorAttachment | TextureUsage::TransferDst | TextureUsage::TransferSrc | m_desc.extraUsage;
        td.name = std::format("swapchain[{}]", i);
        // Not Sampled: avoid consuming bindless slots unless requested.
        m_textures.push_back(m_device.registerExternalTexture(images[i], td, Access::Undefined));
        VkSemaphore sem = VK_NULL_HANDLE;
        OX_VK_CHECK(vkCreateSemaphore(s.device, &si, nullptr, &sem));
        m_device.setDebugName(VK_OBJECT_TYPE_SEMAPHORE, u64(sem), std::format("present[{}]", i));
        m_presentSemaphores.push_back(sem);
    }
    m_dirty = false;
    OX_LOG_INFO("rhi", "swapchain {}x{} {} images, format {}, present mode {}{}{}", extent.width, extent.height, count,
                int(m_format), m_activeMode == PresentMode::VSync ? "fifo" : m_activeMode == PresentMode::Mailbox ? "mailbox" : "immediate",
                m_transformMismatch ? std::format(", display transform 0x{:x} (compositor rotates)", u32(m_currentTransform))
                                    : std::string(),
                m_owner ? ", through a swapchain proxy (frame generation)" : "");
    return true;
}

void Swapchain::recreate() {
    m_device.waitIdle();
    build();
}

void Swapchain::setPresentMode(PresentMode mode) {
    if (mode != m_desc.presentMode) {
        m_desc.presentMode = mode;
        m_dirty = true;
    }
}

bool Swapchain::handleSurfaceChange() {
    ISurfaceProvider* provider = m_device.surfaceProvider();
    // Read the generation first: a window that changes again while we tear down bumps it once more and is
    // handled by the next acquire.
    m_surfaceGeneration = provider ? provider->surfaceGeneration() : 0;
    m_device.waitIdle();
    destroyImages();
    {
        std::lock_guard lock(m_latencyMutex);
        destroySwapchainLocked();
    }
    m_device.releaseSurface(); // the provider drops its reference to the old native window here
    m_extent = {};
    OX_LOG_INFO("rhi", "swapchain released (native window changed)");
    return true;
}

bool Swapchain::suboptimalNeedsRecreate() {
    // Expected with compositor rotation (see build()); size changes still arrive through framebufferSize().
    return !m_transformMismatch;
}

bool Swapchain::acquire() {
    m_acquired = false;
    ISurfaceProvider* provider = m_device.surfaceProvider();
    if (provider && provider->surfaceGeneration() != m_surfaceGeneration) handleSurfaceChange();
    const VkExtent2D fb = provider ? provider->framebufferSize() : VkExtent2D{};
    if (fb.width == 0 || fb.height == 0) return false;
    if (!m_device.surface()) {
        if (!m_device.recreateSurface()) return false;
        m_dirty = true;
    }
    if (m_wantProxy != m_owner) m_dirty = true;
    if (m_dirty || !m_swapchain || fb.width != m_extent.width || fb.height != m_extent.height) {
        recreate();
        if (!m_swapchain || m_extent.width == 0) return false;
    }
    m_acquireSlot = m_device.frameIndex();
    for (int attempt = 0; attempt < 2; ++attempt) {
        const VkResult r = m_owner ? m_owner->acquire(m_swapchain, ~0ull, m_acquireSemaphores[m_acquireSlot], m_imageIndex)
                                   : vkAcquireNextImageKHR(m_device.vkDevice(), m_swapchain, ~0ull,
                                                           m_acquireSemaphores[m_acquireSlot], VK_NULL_HANDLE, &m_imageIndex);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
            if (r == VK_SUBOPTIMAL_KHR && suboptimalNeedsRecreate()) m_dirty = true;
            m_acquired = true;
            // The image's previous contents are irrelevant; the presentation engine released it.
            m_device.setTrackedAccess(m_textures[m_imageIndex], Access::Undefined);
            return true;
        }
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            recreate();
            continue;
        }
        if (r == VK_ERROR_SURFACE_LOST_KHR) {
            handleSurfaceChange(); // a new surface is created on the next acquire
            return false;
        }
        OX_LOG_ERROR("rhi", "vkAcquireNextImageKHR: {}", vkResultName(r));
        return false;
    }
    return false;
}

bool Swapchain::present() {
    if (!m_acquired) return false;
    m_acquired = false;
    VkSemaphore wait = m_presentSemaphores[m_imageIndex];
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    VkPresentIdKHR presentIdInfo{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
    const u64 presentId = m_presentId;
    if (presentId != 0 && m_device.caps().presentId && !m_owner) {
        presentIdInfo.swapchainCount = 1;
        presentIdInfo.pPresentIds = &presentId;
        pi.pNext = &presentIdInfo;
    }
    const bool markers = presentId != 0 && m_latencyCapable && !m_owner; // also with the mode off (latency stats)
    if (markers) setLatencyMarker(presentId, LatencyMarker::PresentStart);
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &wait;
    pi.swapchainCount = 1;
    pi.pSwapchains = &m_swapchain;
    pi.pImageIndices = &m_imageIndex;
    DeviceState& s = m_device.state();
    VkResult r;
    {
        std::lock_guard lock(*s.queue(QueueType::Graphics).mutex);
        r = m_owner ? m_owner->present(s.queue(QueueType::Graphics).queue, pi)
                    : vkQueuePresentKHR(s.queue(QueueType::Graphics).queue, &pi);
    }
    if (markers) setLatencyMarker(presentId, LatencyMarker::PresentEnd);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        if (r == VK_ERROR_OUT_OF_DATE_KHR || suboptimalNeedsRecreate()) m_dirty = true;
        return r == VK_SUBOPTIMAL_KHR;
    }
    if (r == VK_ERROR_SURFACE_LOST_KHR) {
        m_dirty = true;
        OX_LOG_WARN("rhi", "vkQueuePresentKHR: surface lost");
        return false;
    }
    if (r != VK_SUCCESS) {
        OX_LOG_ERROR("rhi", "vkQueuePresentKHR: {}", vkResultName(r));
        return false;
    }
    return true;
}

// ---- low latency (VK_NV_low_latency2) ------------------------------------------------------------------------------

const char* latencyMarkerName(LatencyMarker marker) {
    switch (marker) {
    case LatencyMarker::SimulationStart: return "SimulationStart";
    case LatencyMarker::SimulationEnd: return "SimulationEnd";
    case LatencyMarker::RenderSubmitStart: return "RenderSubmitStart";
    case LatencyMarker::RenderSubmitEnd: return "RenderSubmitEnd";
    case LatencyMarker::PresentStart: return "PresentStart";
    case LatencyMarker::PresentEnd: return "PresentEnd";
    case LatencyMarker::InputSample: return "InputSample";
    }
    return "?";
}

namespace {
f64 spanMs(u64 from, u64 to) { return from && to >= from ? f64(to - from) / 1000.0 : 0.0; }
} // namespace

f64 LatencyFrameTimings::pcLatencyMs() const { return spanMs(simStartUs, gpuRenderEndUs); }
f64 LatencyFrameTimings::gameToRenderMs() const { return spanMs(simStartUs, renderSubmitStartUs); }
f64 LatencyFrameTimings::renderQueueMs() const { return spanMs(renderSubmitEndUs, gpuRenderStartUs); }
f64 LatencyFrameTimings::gpuRenderMs() const { return spanMs(gpuRenderStartUs, gpuRenderEndUs); }

void Swapchain::applyLatencyModeLocked() {
    if (!m_latencyCapable || !m_swapchain || m_owner) return;
    VkLatencySleepModeInfoNV mi{VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV};
    mi.lowLatencyMode = m_latencyMode.enabled ? VK_TRUE : VK_FALSE;
    mi.lowLatencyBoost = m_latencyMode.enabled && m_latencyMode.boost ? VK_TRUE : VK_FALSE;
    mi.minimumIntervalUs = m_latencyMode.minimumIntervalUs;
    if (VkResult r = vkSetLatencySleepModeNV(m_device.vkDevice(), m_swapchain, &mi); r != VK_SUCCESS) {
        OX_LOG_WARN("rhi", "vkSetLatencySleepModeNV: {}", vkResultName(r));
    }
}

void Swapchain::setLowLatencyMode(const LowLatencyMode& mode) {
    if (!m_latencyCapable) return;
    std::lock_guard lock(m_latencyMutex);
    const bool changed = mode.enabled != m_latencyMode.enabled || mode.boost != m_latencyMode.boost ||
                         mode.minimumIntervalUs != m_latencyMode.minimumIntervalUs;
    m_latencyMode = mode;
    m_latencyEnabled.store(mode.enabled, std::memory_order_relaxed);
    if (changed) {
        applyLatencyModeLocked();
        OX_LOG_INFO("rhi", "low latency mode (VK_NV_low_latency2): {}{}{}", mode.enabled ? "on" : "off",
                    mode.enabled && mode.boost ? " + boost" : "",
                    mode.minimumIntervalUs ? std::format(", min interval {} us", mode.minimumIntervalUs) : std::string());
    }
}

LowLatencyMode Swapchain::lowLatencyMode() const {
    std::lock_guard lock(m_latencyMutex);
    return m_latencyMode;
}

f64 Swapchain::latencySleep(u32 timeoutMs) {
    if (!lowLatencyEnabled()) return -1.0;
    u64 value = 0;
    {
        std::lock_guard lock(m_latencyMutex);
        if (!m_swapchain || m_owner) return -1.0;
        value = ++m_latencySleepValue;
        VkLatencySleepInfoNV si{VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV};
        si.signalSemaphore = m_latencySemaphore;
        si.value = value;
        if (VkResult r = vkLatencySleepNV(m_device.vkDevice(), m_swapchain, &si); r != VK_SUCCESS) {
            OX_LOG_WARN("rhi", "vkLatencySleepNV: {}", vkResultName(r));
            return -1.0;
        }
    }
    m_sleepCount.fetch_add(1, std::memory_order_relaxed);
    const auto t0 = std::chrono::steady_clock::now();
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &m_latencySemaphore;
    wi.pValues = &value;
    const VkResult r = vkWaitSemaphores(m_device.vkDevice(), &wi, u64(timeoutMs) * 1'000'000ull);
    if (r == VK_TIMEOUT) OX_LOG_DEBUG("rhi", "vkLatencySleepNV: no signal after {} ms", timeoutMs);
    return std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void Swapchain::setLatencyMarker(u64 presentId, LatencyMarker marker) {
    if (!m_latencyCapable || presentId == 0) return;
    std::lock_guard lock(m_latencyMutex);
    if (!m_swapchain || m_owner) return;
    VkSetLatencyMarkerInfoNV mi{VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV};
    mi.presentID = presentId;
    mi.marker = VkLatencyMarkerNV(marker);
    vkSetLatencyMarkerNV(m_device.vkDevice(), m_swapchain, &mi);
}

std::vector<LatencyFrameTimings> Swapchain::latencyTimings() const {
    std::vector<LatencyFrameTimings> out;
    if (!m_latencyCapable) return out;
    std::lock_guard lock(m_latencyMutex);
    if (!m_swapchain || m_owner) return out;
    VkGetLatencyMarkerInfoNV info{VK_STRUCTURE_TYPE_GET_LATENCY_MARKER_INFO_NV};

    vkGetLatencyTimingsNV(m_device.vkDevice(), m_swapchain, &info); // pTimings null: timingCount = available
    if (info.timingCount == 0) return out;
    std::vector<VkLatencyTimingsFrameReportNV> reports(std::min(info.timingCount, 64u));
    for (auto& r : reports) r = {VK_STRUCTURE_TYPE_LATENCY_TIMINGS_FRAME_REPORT_NV};
    info.timingCount = u32(reports.size());
    info.pTimings = reports.data();
    vkGetLatencyTimingsNV(m_device.vkDevice(), m_swapchain, &info);
    out.reserve(info.timingCount);
    for (u32 i = 0; i < info.timingCount && i < reports.size(); ++i) {
        const VkLatencyTimingsFrameReportNV& r = reports[i];
        if (r.presentID == 0) continue;
        out.push_back({r.presentID, r.inputSampleTimeUs, r.simStartTimeUs, r.simEndTimeUs, r.renderSubmitStartTimeUs,
                       r.renderSubmitEndTimeUs, r.presentStartTimeUs, r.presentEndTimeUs, r.driverStartTimeUs,
                       r.driverEndTimeUs, r.osRenderQueueStartTimeUs, r.osRenderQueueEndTimeUs, r.gpuRenderStartTimeUs,
                       r.gpuRenderEndTimeUs});
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.presentId < b.presentId; });
    return out;
}

TextureHandle Swapchain::currentTexture() const { return m_textures.empty() ? TextureHandle{} : m_textures[m_imageIndex]; }
VkSemaphore Swapchain::acquireSemaphore() const { return m_acquireSemaphores[m_acquireSlot]; }
VkSemaphore Swapchain::presentSemaphore() const { return m_presentSemaphores[m_imageIndex]; }

} // namespace ox::rhi
