#pragma once

// VK_KHR_android_surface provider for an ANativeWindow (GameActivity / NativeActivity). Android only
// (target Oxwald::rhi_android).
//
// Android destroys the native window whenever the activity leaves the screen (pause, multi-window changes, some
// rotations) and hands out a new one on resume. The platform layer calls setWindow(nullptr) /
// setWindow(newWindow); surfaceGeneration() changes and the swapchain (render thread) drops its VkSurfaceKHR +
// VkSwapchainKHR and later creates new ones. Every VkSurfaceKHR keeps its own ANativeWindow reference until
// destroySurface(), so a window the system already gave up stays a valid object until Vulkan let go of it;
// waitForRelease() lets APP_CMD_TERM_WINDOW wait (bounded) for that to happen, as the platform contract asks.

#include <oxwald/rhi/swapchain.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <unordered_map>

struct ANativeWindow;

namespace ox::rhi {

class AndroidSurfaceProvider final : public ISurfaceProvider {
public:
    AndroidSurfaceProvider() = default;
    ~AndroidSurfaceProvider() override;
    AndroidSurfaceProvider(const AndroidSurfaceProvider&) = delete;
    AndroidSurfaceProvider& operator=(const AndroidSurfaceProvider&) = delete;

    // Game/main thread. nullptr = the window was destroyed. Takes its own reference (ANativeWindow_acquire).
    void setWindow(ANativeWindow* window);
    [[nodiscard]] ANativeWindow* window() const;
    // Blocks until no VkSurfaceKHR references `window` any more, or the timeout expires. Returns true when released.
    bool waitForRelease(ANativeWindow* window, std::chrono::milliseconds timeout);

    [[nodiscard]] std::vector<const char*> requiredInstanceExtensions() const override;
    VkSurfaceKHR createSurface(VkInstance instance) override;
    [[nodiscard]] VkExtent2D framebufferSize() const override;
    void destroySurface(VkInstance instance, VkSurfaceKHR surface) override;
    [[nodiscard]] u64 surfaceGeneration() const override { return m_generation.load(std::memory_order_acquire); }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_released;
    ANativeWindow* m_window = nullptr;
    std::unordered_map<VkSurfaceKHR, ANativeWindow*> m_surfaceWindows;
    std::atomic<u64> m_generation{1};
};

} // namespace ox::rhi
