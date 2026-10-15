#pragma once

#include <oxwald/rhi/types.hpp>

#include <atomic>
#include <memory>
#include <mutex>
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
    // Changes whenever the native window behind the surface was destroyed or replaced (Android destroys the
    // ANativeWindow on pause and creates a new one on resume; rotation may do the same). The swapchain then drops
    // its VkSurfaceKHR + VkSwapchainKHR and, once framebufferSize() is non-zero again, creates new ones through
    // createSurface(). Desktop windows keep their surface for life (0). Must be thread-safe (render thread).
    [[nodiscard]] virtual u64 surfaceGeneration() const { return 0; }
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
    // Create the swapchain with VkSwapchainLatencyCreateInfoNV when DeviceCaps::lowLatency2 (NVIDIA Reflex). The
    // latency mode itself stays off until setLowLatencyMode().
    bool lowLatency = true;
};

// NVIDIA Reflex (VK_NV_low_latency2). Markers are tagged with the frame's present id (Swapchain::setPresentId on the
// render thread; the game thread uses the same id for the frame it simulates). Order within a frame:
// [latencySleep] SimulationStart, InputSample, SimulationEnd, RenderSubmitStart, RenderSubmitEnd, PresentStart,
// PresentEnd (the last two are emitted by Swapchain::present()).
enum class LatencyMarker : u8 {
    SimulationStart = 0, // VK_LATENCY_MARKER_SIMULATION_START_NV
    SimulationEnd = 1,
    RenderSubmitStart = 2,
    RenderSubmitEnd = 3,
    PresentStart = 4,
    PresentEnd = 5,
    InputSample = 6,
};
[[nodiscard]] const char* latencyMarkerName(LatencyMarker marker);

struct LowLatencyMode {
    bool enabled = false;      // lowLatencyMode: the driver paces latencySleep() so the CPU starts just in time
    bool boost = false;        // lowLatencyBoost: keep GPU clocks high even when CPU bound
    u32 minimumIntervalUs = 0; // driver frame limiter (0 = off)
};

// One VkLatencyTimingsFrameReportNV, microseconds of the driver's clock (only differences are meaningful).
struct LatencyFrameTimings {
    u64 presentId = 0;
    u64 inputSampleUs = 0, simStartUs = 0, simEndUs = 0, renderSubmitStartUs = 0, renderSubmitEndUs = 0;
    u64 presentStartUs = 0, presentEndUs = 0, driverStartUs = 0, driverEndUs = 0;
    u64 osRenderQueueStartUs = 0, osRenderQueueEndUs = 0, gpuRenderStartUs = 0, gpuRenderEndUs = 0;
    // Simulation start -> GPU render end: NVIDIA's "PC latency" without the display / peripheral part.
    [[nodiscard]] f64 pcLatencyMs() const;
    [[nodiscard]] f64 gameToRenderMs() const;  // simulation start -> render submit start
    [[nodiscard]] f64 renderQueueMs() const;   // render submit end -> GPU render start (frames queued ahead)
    [[nodiscard]] f64 gpuRenderMs() const;     // GPU render start -> end
    [[nodiscard]] bool complete() const { return simStartUs && gpuRenderEndUs >= simStartUs; }
};

// Takes over the VkSwapchainKHR entry points: AMD FSR 3 frame generation's swapchain proxy (render module,
// features/framegen). The handles it returns are only valid with these functions; Reflex (VK_NV_low_latency2) and
// VkPresentIdKHR are not used while a proxy owns the swapchain.
class ISwapchainProxy {
public:
    virtual ~ISwapchainProxy() = default;
    virtual VkResult createSwapchain(const VkSwapchainCreateInfoKHR& info, VkSwapchainKHR& out) = 0;
    virtual void destroySwapchain(VkSwapchainKHR swapchain) = 0;
    virtual VkResult getImages(VkSwapchainKHR swapchain, u32& count, VkImage* images) = 0;
    virtual VkResult acquire(VkSwapchainKHR swapchain, u64 timeout, VkSemaphore semaphore, u32& imageIndex) = 0;
    // Called with the graphics queue mutex held (the proxy may submit its own work to `queue` here).
    virtual VkResult present(VkQueue queue, const VkPresentInfoKHR& info) = 0;
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
    // Render thread. Routes the swapchain through `proxy` (nullptr = plain Vulkan) from the next acquire on (the
    // swapchain is recreated). The proxy must outlive its swapchain: setProxy(nullptr) + acquire, or destroy the
    // Swapchain first.
    void setProxy(ISwapchainProxy* proxy);
    [[nodiscard]] ISwapchainProxy* activeProxy() const { return m_owner; }

    // --- low latency (NVIDIA Reflex, VK_NV_low_latency2). All no-ops when lowLatencySupported() is false. ---
    // The device has VK_NV_low_latency2 and the swapchain was created with latency mode.
    [[nodiscard]] bool lowLatencySupported() const { return m_latencyCapable && !m_ownerIsProxy.load(std::memory_order_relaxed); }
    // VK_NV_low_latency2 on the device, whether or not a proxy currently hides it.
    [[nodiscard]] bool lowLatencyCapable() const { return m_latencyCapable; }
    [[nodiscard]] bool lowLatencyEnabled() const { return lowLatencySupported() && m_latencyEnabled.load(std::memory_order_relaxed); }
    // Any thread. Re-applied automatically to recreated swapchains.
    void setLowLatencyMode(const LowLatencyMode& mode);
    [[nodiscard]] LowLatencyMode lowLatencyMode() const;
    // Any thread (the game thread, right before simulation start): vkLatencySleepNV + host wait on its timeline
    // semaphore (at most timeoutMs). Returns the time slept in ms, or a negative value when nothing was done.
    f64 latencySleep(u32 timeoutMs = 200);
    // Any thread. presentId must be > 0 and match the id passed to setPresentId() for the same frame.
    void setLatencyMarker(u64 presentId, LatencyMarker marker);
    // Any thread: the driver's reports of the last frames (oldest first; at most 64).
    [[nodiscard]] std::vector<LatencyFrameTimings> latencyTimings() const;
    // Render thread, before the frame's final submit: the present id of the frame being rendered (0 = none). Used
    // for VkLatencySubmissionPresentIdNV (Device::submit), VkPresentIdKHR and the present markers. Must increase.
    void setPresentId(u64 id) { m_presentId = id; }
    [[nodiscard]] u64 presentId() const { return m_presentId; }
    // Number of vkLatencySleepNV / vkSetLatencyMarkerNV calls (tests).
    [[nodiscard]] u64 latencySleepCount() const { return m_sleepCount.load(); }

    [[nodiscard]] TextureHandle currentTexture() const;
    [[nodiscard]] u32 currentImageIndex() const { return m_imageIndex; }
    [[nodiscard]] VkFormat format() const { return m_format; }
    [[nodiscard]] VkColorSpaceKHR colorSpace() const { return m_colorSpace; }
    [[nodiscard]] VkExtent2D extent() const { return m_extent; }
    [[nodiscard]] PresentMode presentMode() const { return m_activeMode; }
    // Surface transform of the display (VkSurfaceCapabilitiesKHR::currentTransform at the last build) and the one
    // the swapchain was created with. The swapchain always renders upright (IDENTITY) and lets the compositor
    // rotate; on Android a ROTATE_90/270 display then makes every present return VK_SUBOPTIMAL_KHR, which is
    // expected and not treated as "recreate" (see m_transformMismatch).
    [[nodiscard]] VkSurfaceTransformFlagBitsKHR currentTransform() const { return m_currentTransform; }
    [[nodiscard]] VkSurfaceTransformFlagBitsKHR preTransform() const { return m_preTransform; }
    [[nodiscard]] u32 imageCount() const { return u32(m_textures.size()); }
    [[nodiscard]] VkSwapchainKHR vk() const { return m_swapchain; }

    // Used by Device::submit.
    [[nodiscard]] VkSemaphore acquireSemaphore() const;
    [[nodiscard]] VkSemaphore presentSemaphore() const;

private:
    explicit Swapchain(Device& device, const SwapchainDesc& desc);
    bool build();
    void destroyImages();
    // Surface generation changed (native window lost / replaced): drop swapchain + surface, rebuild when possible.
    bool handleSurfaceChange();
    bool suboptimalNeedsRecreate();

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
    u64 m_surfaceGeneration = 0;
    VkSurfaceTransformFlagBitsKHR m_currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    VkSurfaceTransformFlagBitsKHR m_preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    bool m_transformMismatch = false;

    // low latency: m_latencyMutex guards m_swapchain against replacement while another thread calls a latency
    // function (build()/handleSurfaceChange() take it around the handle swap).
    mutable std::mutex m_latencyMutex;
    bool m_latencyCapable = false;
    std::atomic<bool> m_latencyEnabled{false};
    LowLatencyMode m_latencyMode;          // m_latencyMutex
    VkSemaphore m_latencySemaphore = VK_NULL_HANDLE; // timeline, signalled by vkLatencySleepNV
    u64 m_latencySleepValue = 0;           // m_latencyMutex
    std::atomic<u64> m_sleepCount{0};
    u64 m_presentId = 0;                   // render thread
    void applyLatencyModeLocked();
    void destroySwapchainLocked(); // via its owner (Vulkan or the proxy); m_latencyMutex held
    ISwapchainProxy* m_wantProxy = nullptr; // render thread
    ISwapchainProxy* m_owner = nullptr;     // created m_swapchain (nullptr = vkCreateSwapchainKHR)
    std::atomic<bool> m_ownerIsProxy{false};

};

} // namespace ox::rhi
