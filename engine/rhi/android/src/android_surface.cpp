#include <oxwald/core/log.hpp>
#include <oxwald/rhi/android_surface.hpp>
#include <oxwald/rhi/environment.hpp>

#include <android/native_window.h>
// volk is built without platform defines: declare the Android surface types/entry point here and load the
// function through vkGetInstanceProcAddr.
#include <vulkan/vulkan_android.h>

#include <algorithm>

namespace ox::rhi {

AndroidSurfaceProvider::~AndroidSurfaceProvider() {
    std::lock_guard lock(m_mutex);
    for (auto& [surface, window] : m_surfaceWindows) {
        if (window) ANativeWindow_release(window);
    }
    m_surfaceWindows.clear();
    if (m_window) ANativeWindow_release(m_window);
    m_window = nullptr;
}

void AndroidSurfaceProvider::setWindow(ANativeWindow* window) {
    {
        std::lock_guard lock(m_mutex);
        if (window == m_window) return;
        if (window) ANativeWindow_acquire(window);
        if (m_window) ANativeWindow_release(m_window);
        m_window = window;
    }
    m_generation.fetch_add(1, std::memory_order_acq_rel);
}

ANativeWindow* AndroidSurfaceProvider::window() const {
    std::lock_guard lock(m_mutex);
    return m_window;
}

bool AndroidSurfaceProvider::waitForRelease(ANativeWindow* window, std::chrono::milliseconds timeout) {
    std::unique_lock lock(m_mutex);
    return m_released.wait_for(lock, timeout, [&] {
        return std::none_of(m_surfaceWindows.begin(), m_surfaceWindows.end(),
                            [&](const auto& entry) { return entry.second == window; });
    });
}

std::vector<const char*> AndroidSurfaceProvider::requiredInstanceExtensions() const {
    return {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
}

VkSurfaceKHR AndroidSurfaceProvider::createSurface(VkInstance instance) {
    auto create = reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(vkGetInstanceProcAddr(instance, "vkCreateAndroidSurfaceKHR"));
    if (!create) {
        OX_LOG_ERROR("rhi", "vkCreateAndroidSurfaceKHR not available (VK_KHR_android_surface not enabled?)");
        return VK_NULL_HANDLE;
    }
    std::lock_guard lock(m_mutex);
    if (!m_window) return VK_NULL_HANDLE;
    VkAndroidSurfaceCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    ci.window = m_window;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (VkResult r = create(instance, &ci, nullptr, &surface); r != VK_SUCCESS) {
        OX_LOG_ERROR("rhi", "vkCreateAndroidSurfaceKHR failed ({})", int(r));
        return VK_NULL_HANDLE;
    }
    ANativeWindow_acquire(m_window);
    m_surfaceWindows[surface] = m_window;
    OX_LOG_INFO("rhi", "Android surface created for window {}x{}", ANativeWindow_getWidth(m_window),
                ANativeWindow_getHeight(m_window));
    return surface;
}

VkExtent2D AndroidSurfaceProvider::framebufferSize() const {
    std::lock_guard lock(m_mutex);
    if (!m_window) return {0, 0};
    const int w = ANativeWindow_getWidth(m_window);
    const int h = ANativeWindow_getHeight(m_window);
    return {u32(w > 0 ? w : 0), u32(h > 0 ? h : 0)};
}

void AndroidSurfaceProvider::destroySurface(VkInstance instance, VkSurfaceKHR surface) {
    if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
    {
        std::lock_guard lock(m_mutex);
        if (auto it = m_surfaceWindows.find(surface); it != m_surfaceWindows.end()) {
            if (it->second) ANativeWindow_release(it->second);
            m_surfaceWindows.erase(it);
        }
    }
    m_released.notify_all();
}

} // namespace ox::rhi
