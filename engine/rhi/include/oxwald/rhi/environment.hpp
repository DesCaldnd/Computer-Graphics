#pragma once

#include <oxwald/rhi/vulkan.hpp>

#include <filesystem>
#include <string>

namespace ox::rhi {

// Points the Khronos loader at the bundled driver/layers before it is loaded:
//  * macOS: VK_DRIVER_FILES -> MoltenVK ICD json, VK_ADD_LAYER_PATH -> validation layer manifest dir.
//  * packaged builds use <exe dir>/vulkan/ (see ox_deploy_vulkan_runtime), dev builds the vcpkg_installed paths.
// Variables already set by the user are left untouched. Safe to call many times.
void configureVulkanEnvironment();

// configureVulkanEnvironment() + load the loader library + volkInitialize. Idempotent; false if no loader found.
bool initializeVulkanLoader();
PFN_vkGetInstanceProcAddr loaderGetInstanceProcAddr();
std::string loaderLibraryPath(); // which library was loaded (for logs)

std::filesystem::path executableDirectory();

} // namespace ox::rhi
