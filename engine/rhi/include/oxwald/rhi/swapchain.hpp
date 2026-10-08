#pragma once

#include <oxwald/rhi/types.hpp>

#include <memory>
#include <vector>

namespace ox::rhi {

class Device;

// Windowing-system bridge. The Qt editor implements it with QVulkanInstance/QWindow, the player with GLFW
// (oxwald/rhi/glfw_surface.hpp, target Oxwald::rhi_glfw). The device asks for instance extensions before
// creating the instance and calls createSurface() right after.
class ISurfaceProvider {
public:
    virtual ~ISurfaceProvider() = default;
    [[nodiscard]] virtual std::vector<const char*> requiredInstanceExtensions() const = 0;
    virtual VkSurfaceKHR createSurface(VkInstance instance) = 0;
    // Current drawable size in pixels (0×0 while minimized).
    [[nodiscard]] virtual VkExtent2D framebufferSize() const = 0;
    // Called before the instance is destroyed; the default destroys the surface with vkDestroySurfaceKHR.
    virtual void destroySurface(VkInstance instance, VkSurfaceKHR surface);
};

enum class PresentMode : u8 {
    VSync,     // FIFO — always available
    Mailbox,   // low latency vsync, falls back to VSync
    Immediate, // tearing allowed, falls back to Mailbox → VSync
};

enum class SwapchainColor : u8 {
    Srgb,    // 8-bit sRGB surface (shaders write linear)
    Unorm,   // 8-bit UNORM (shaders do their own encoding)
    Hdr10,   // A2B10G10R10 + ST.2084, falls back to Srgb
    ScRgb,   // RGBA16F extended linear sRGB, falls back to Srgb
};

struct SwapchainDesc {
    PresentMode presentMode = PresentMode::VSync;
    SwapchainColor color = SwapchainColor::Srgb;
    u32 imageCount = 3;
    TextureUsage extraUsage = TextureUsage::None; // ColorAttachment|TransferDst always set
};

class Swapchain {
public:
    // Uses the device's surface (DeviceDesc::surface must have been set).
    static std::unique_ptr<Swapchain> create(Device& device, const SwapchainDesc& desc = {});
    ~Swapchain();
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    // Acquires the next image for the current frame. Returns false when nothing should be rendered this frame
    // (window minimized); recreates the swapchain transparently on resize / VK_ERROR_OUT_OF_DATE_KHR.
    bool acquire();
    // Presents the acquired image; the graphics submit of this frame must have used SubmitInfo::swapchain.
    bool present();
    void recreate();
    void setPresentMode(PresentMode mode); // applied on the next acquire

    [[nodiscard]] TextureHandle currentTexture() const;
    [[nodiscard]] u32 currentImageIndex() const { return m_imageIndex; }
    [[nodiscard]] VkFormat format() const { return m_format; }
    [[nodiscard]] VkColorSpaceKHR colorSpace() const { return m_colorSpace; }
    [[nodiscard]] VkExtent2D extent() const { return m_extent; }
    [[nodiscard]] PresentMode presentMode() const { return m_activeMode; }
    [[nodiscard]] u32 imageCount() const { return u32(m_textures.size()); }
    [[nodiscard]] VkSwapchainKHR vk() const { return m_swapchain; }

    // Used by Device::submit.
    [[nodiscard]] VkSemaphore acquireSemaphore() const;
    [[nodiscard]] VkSemaphore presentSemaphore() const;

private:
    explicit Swapchain(Device& device, const SwapchainDesc& desc);
    bool build();
    void destroyImages();

    Device& m_device;
    SwapchainDesc m_desc;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat m_format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR m_colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D m_extent{};
    PresentMode m_activeMode = PresentMode::VSync;
    bool m_dirty = false;
    bool m_acquired = false;
    u32 m_imageIndex = 0;
    std::vector<TextureHandle> m_textures;
    std::vector<VkSemaphore> m_acquireSemaphores; // per frame in flight
    std::vector<VkSemaphore> m_presentSemaphores; // per swapchain image
    u32 m_acquireSlot = 0;
};

} // namespace ox::rhi
