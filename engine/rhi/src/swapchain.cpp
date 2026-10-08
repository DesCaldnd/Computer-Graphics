#include "device_impl.hpp"

#include <oxwald/core/assert.hpp>
#include <oxwald/core/log.hpp>

#include <algorithm>

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
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    m_acquireSemaphores.resize(device.framesInFlight());
    for (auto& s : m_acquireSemaphores) {
        OX_VK_CHECK(vkCreateSemaphore(device.vkDevice(), &si, nullptr, &s));
    }
}

Swapchain::~Swapchain() {
    m_device.waitIdle();
    destroyImages();
    for (VkSemaphore s : m_acquireSemaphores) vkDestroySemaphore(m_device.vkDevice(), s, nullptr);
    if (m_swapchain) vkDestroySwapchainKHR(m_device.vkDevice(), m_swapchain, nullptr);
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
    VkSurfaceCapabilitiesKHR caps{};
    OX_VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s.physical, s.surface, &caps));
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
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                            : VkCompositeAlphaFlagBitsKHR(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
    ci.presentMode = presentMode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = m_swapchain;

    VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
    if (VkResult r = vkCreateSwapchainKHR(s.device, &ci, nullptr, &newSwapchain); r != VK_SUCCESS) {
        OX_LOG_ERROR("rhi", "vkCreateSwapchainKHR: {}", vkResultName(r));
        return false;
    }
    if (m_swapchain) {
        destroyImages();
        vkDestroySwapchainKHR(s.device, m_swapchain, nullptr);
    }
    m_swapchain = newSwapchain;
    m_format = format.format;
    m_colorSpace = format.colorSpace;
    m_extent = extent;

    vkGetSwapchainImagesKHR(s.device, m_swapchain, &count, nullptr);
    std::vector<VkImage> images(count);
    vkGetSwapchainImagesKHR(s.device, m_swapchain, &count, images.data());
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
    OX_LOG_INFO("rhi", "swapchain {}x{} {} images, format {}, present mode {}", extent.width, extent.height, count,
                int(m_format), m_activeMode == PresentMode::VSync ? "fifo" : m_activeMode == PresentMode::Mailbox ? "mailbox" : "immediate");
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

bool Swapchain::acquire() {
    m_acquired = false;
    const VkExtent2D fb = m_device.surfaceProvider()->framebufferSize();
    if (fb.width == 0 || fb.height == 0) return false;
    if (m_dirty || !m_swapchain || fb.width != m_extent.width || fb.height != m_extent.height) {
        recreate();
        if (!m_swapchain || m_extent.width == 0) return false;
    }
    m_acquireSlot = m_device.frameIndex();
    for (int attempt = 0; attempt < 2; ++attempt) {
        const VkResult r = vkAcquireNextImageKHR(m_device.vkDevice(), m_swapchain, ~0ull, m_acquireSemaphores[m_acquireSlot],
                                                 VK_NULL_HANDLE, &m_imageIndex);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
            if (r == VK_SUBOPTIMAL_KHR) m_dirty = true;
            m_acquired = true;
            // The image's previous contents are irrelevant; the presentation engine released it.
            m_device.setTrackedAccess(m_textures[m_imageIndex], Access::Undefined);
            return true;
        }
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            recreate();
            continue;
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
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &wait;
    pi.swapchainCount = 1;
    pi.pSwapchains = &m_swapchain;
    pi.pImageIndices = &m_imageIndex;
    DeviceState& s = m_device.state();
    VkResult r;
    {
        std::lock_guard lock(*s.queue(QueueType::Graphics).mutex);
        r = vkQueuePresentKHR(s.queue(QueueType::Graphics).queue, &pi);
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        m_dirty = true;
        return r == VK_SUBOPTIMAL_KHR;
    }
    if (r != VK_SUCCESS) {
        OX_LOG_ERROR("rhi", "vkQueuePresentKHR: {}", vkResultName(r));
        return false;
    }
    return true;
}

TextureHandle Swapchain::currentTexture() const { return m_textures.empty() ? TextureHandle{} : m_textures[m_imageIndex]; }
VkSemaphore Swapchain::acquireSemaphore() const { return m_acquireSemaphores[m_acquireSlot]; }
VkSemaphore Swapchain::presentSemaphore() const { return m_presentSemaphores[m_imageIndex]; }

} // namespace ox::rhi
