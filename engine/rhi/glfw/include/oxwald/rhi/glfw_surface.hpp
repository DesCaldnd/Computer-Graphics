#pragma once

#include <oxwald/rhi/swapchain.hpp>

struct GLFWwindow;

namespace ox::rhi {

// Must be called before glfwInit(): loads the Vulkan loader through rhi (bundled MoltenVK on macOS) and hands
// vkGetInstanceProcAddr to GLFW so it never searches for a system loader itself.
bool initGlfwVulkan();

// ISurfaceProvider for a GLFW window created with GLFW_CLIENT_API = GLFW_NO_API.
class GlfwSurfaceProvider final : public ISurfaceProvider {
public:
    explicit GlfwSurfaceProvider(GLFWwindow* window) : m_window(window) {}
    [[nodiscard]] std::vector<const char*> requiredInstanceExtensions() const override;
    VkSurfaceKHR createSurface(VkInstance instance) override;
    [[nodiscard]] VkExtent2D framebufferSize() const override;
    [[nodiscard]] GLFWwindow* window() const { return m_window; }

private:
    GLFWwindow* m_window;
};

} // namespace ox::rhi
