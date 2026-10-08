#include <oxwald/core/log.hpp>
#include <oxwald/rhi/environment.hpp>
#include <oxwald/rhi/glfw_surface.hpp>

// volk.h (via environment.hpp) defines VK_VERSION_1_0, which makes glfw3.h declare its Vulkan entry points.
#include <GLFW/glfw3.h>

namespace ox::rhi {

bool initGlfwVulkan() {
    if (!initializeVulkanLoader()) return false;
    glfwInitVulkanLoader(loaderGetInstanceProcAddr());
    return true;
}

std::vector<const char*> GlfwSurfaceProvider::requiredInstanceExtensions() const {
    u32 count = 0;
    const char** exts = glfwGetRequiredInstanceExtensions(&count);
    if (!exts) {
        OX_LOG_ERROR("rhi", "GLFW reports no Vulkan support (was initGlfwVulkan() called before glfwInit()?)");
        return {};
    }
    return {exts, exts + count};
}

VkSurfaceKHR GlfwSurfaceProvider::createSurface(VkInstance instance) {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    const VkResult r = glfwCreateWindowSurface(instance, m_window, nullptr, &surface);
    if (r != VK_SUCCESS) {
        OX_LOG_ERROR("rhi", "glfwCreateWindowSurface failed ({})", int(r));
        return VK_NULL_HANDLE;
    }
    return surface;
}

VkExtent2D GlfwSurfaceProvider::framebufferSize() const {
    int w = 0, h = 0;
    glfwGetFramebufferSize(m_window, &w, &h);
    return {u32(w > 0 ? w : 0), u32(h > 0 ? h : 0)};
}

} // namespace ox::rhi
