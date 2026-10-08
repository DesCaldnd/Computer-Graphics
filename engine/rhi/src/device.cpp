#include "device_impl.hpp"

#include <oxwald/core/assert.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/rhi/environment.hpp>
#include <oxwald/rhi/format.hpp>

#include <VkBootstrap.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>

#ifndef OX_RHI_VALIDATION_DEFAULT
#define OX_RHI_VALIDATION_DEFAULT 1
#endif

namespace ox::rhi {

namespace fs = std::filesystem;
using namespace detail;

namespace {

std::atomic<u32> g_validationErrors{0};
std::atomic<u32> g_validationWarnings{0};

bool isIgnoredValidationMessage(const VkDebugUtilsMessengerCallbackDataEXT* data) {
    // Messages that are expected on portability drivers and not actionable.
    static constexpr const char* kIgnored[] = {
        "VUID-VkInstanceCreateInfo-pNext-pNext", // loader-internal chains on some layer versions
    };
    if (!data->pMessageIdName) return false;
    for (const char* id : kIgnored) {
        if (std::strcmp(data->pMessageIdName, id) == 0) return true;
    }
    return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT types,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (isIgnoredValidationMessage(data)) return VK_FALSE;
    const char* msg = data->pMessage ? data->pMessage : "";
    if (std::strstr(msg, "MTLCounterSampleBuffer")) {
        // MoltenVK falls back to emulated timestamps for large query pools (Tracy uses 64K queries): not an error.
        OX_LOG_DEBUG("vulkan", "{}", msg);
        return VK_FALSE;
    }
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++g_validationErrors;
        OX_LOG_ERROR("vulkan", "{}", msg);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        if (types & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) {
            OX_LOG_DEBUG("vulkan", "perf: {}", msg);
        } else {
            ++g_validationWarnings;
            OX_LOG_WARN("vulkan", "{}", msg);
        }
    } else {
        OX_LOG_TRACE("vulkan", "{}", msg);
    }
    return VK_FALSE;
}

bool envFlag(const char* name, bool fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    return !(v[0] == '0' || v[0] == 'f' || v[0] == 'F' || v[0] == 'n' || v[0] == 'N');
}

bool hasExtension(const std::vector<VkExtensionProperties>& exts, const char* name) {
    for (const auto& e : exts) {
        if (std::strcmp(e.extensionName, name) == 0) return true;
    }
    return false;
}

std::vector<u8> readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

} // namespace

// Implemented in profiling.cpp
namespace detail {
void createTracyContext(Device& device, DeviceState& s);
void destroyTracyContext(DeviceState& s);
void collectTracy(Device& device, DeviceState& s);
void readTimestamps(DeviceState& s, FrameContext& f);
void createDefaultSamplers(Device& device, DeviceState& s);
void createBindless(DeviceState& s);
void destroyAllResources(Device& device, DeviceState& s);
} // namespace detail

const char* detail::vkResultName(VkResult r) {
    switch (r) {
#define OX_R(x) case x: return #x;
        OX_R(VK_SUCCESS) OX_R(VK_NOT_READY) OX_R(VK_TIMEOUT) OX_R(VK_INCOMPLETE) OX_R(VK_SUBOPTIMAL_KHR)
        OX_R(VK_ERROR_OUT_OF_HOST_MEMORY) OX_R(VK_ERROR_OUT_OF_DEVICE_MEMORY) OX_R(VK_ERROR_INITIALIZATION_FAILED)
        OX_R(VK_ERROR_DEVICE_LOST) OX_R(VK_ERROR_MEMORY_MAP_FAILED) OX_R(VK_ERROR_LAYER_NOT_PRESENT)
        OX_R(VK_ERROR_EXTENSION_NOT_PRESENT) OX_R(VK_ERROR_FEATURE_NOT_PRESENT) OX_R(VK_ERROR_INCOMPATIBLE_DRIVER)
        OX_R(VK_ERROR_TOO_MANY_OBJECTS) OX_R(VK_ERROR_FORMAT_NOT_SUPPORTED) OX_R(VK_ERROR_SURFACE_LOST_KHR)
        OX_R(VK_ERROR_OUT_OF_DATE_KHR) OX_R(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) OX_R(VK_ERROR_FRAGMENTATION)
        OX_R(VK_ERROR_OUT_OF_POOL_MEMORY) OX_R(VK_ERROR_UNKNOWN)
#undef OX_R
    default: return "VkResult(?)";
    }
}

u32 Device::validationErrorCount() { return g_validationErrors.load(); }
u32 Device::validationWarningCount() { return g_validationWarnings.load(); }
void Device::resetValidationCounters() {
    g_validationErrors = 0;
    g_validationWarnings = 0;
}

void ISurfaceProvider::destroySurface(VkInstance instance, VkSurfaceKHR surface) {
    if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
}

Device::Device() : m_s(std::make_unique<DeviceState>()) {}

std::unique_ptr<Device> Device::create(const DeviceDesc& desc, std::string* error) {
    std::unique_ptr<Device> device(new Device());
    std::string err;
    if (!device->init(desc, err)) {
        OX_LOG_ERROR("rhi", "device creation failed: {}", err);
        if (error) *error = err;
        return nullptr;
    }
    return device;
}

bool Device::init(const DeviceDesc& desc, std::string& error) {
    DeviceState& s = *m_s;
    s.desc = desc;
    s.desc.framesInFlight = std::clamp(desc.framesInFlight, 1u, 3u);
    s.surfaceProvider = desc.surface;

    if (!initializeVulkanLoader()) {
        error = "Vulkan loader not found (set VK_DRIVER_FILES / install a Vulkan driver)";
        return false;
    }

    // ---------------- instance ----------------
    const bool validation = envFlag("OX_VULKAN_VALIDATION", desc.validation.value_or(OX_RHI_VALIDATION_DEFAULT != 0));
    // Ask for 1.3 when the loader has it: MoltenVK caps the reported device version at the instance version.
    u32 loaderVersion = VK_API_VERSION_1_2;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&loaderVersion);
    const u32 instanceApi = loaderVersion >= VK_API_VERSION_1_3 ? VK_API_VERSION_1_3 : VK_API_VERSION_1_2;
    vkb::InstanceBuilder ib(vkGetInstanceProcAddr);
    ib.set_app_name(desc.appName.c_str())
        .set_engine_name("OxwaldEngine")
        .require_api_version(instanceApi)
        .set_minimum_instance_version(1, 2, 0)
        .set_headless(true); // surface extensions are added explicitly from the provider
    if (validation) {
        ib.request_validation_layers(true)
            .set_debug_callback(&debugCallback)
            .set_debug_messenger_severity(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
            .set_debug_messenger_type(VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                      VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT);
    }
    auto sysInfo = vkb::SystemInfo::get_system_info(vkGetInstanceProcAddr);
    if (!sysInfo) {
        error = "vkb::SystemInfo failed: " + sysInfo.error().message();
        return false;
    }
    if (sysInfo->is_extension_available(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        ib.enable_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        s.debugUtils = true;
    }
    if (desc.surface) {
        for (const char* ext : desc.surface->requiredInstanceExtensions()) {
            ib.enable_extension(ext);
        }
        if (sysInfo->is_extension_available(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME)) {
            ib.enable_extension(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
        }
    }
    auto instRet = ib.build();
    if (!instRet) {
        error = "instance: " + instRet.error().message();
        return false;
    }
    vkb::Instance vkbInstance = instRet.value();
    s.instance = vkbInstance.instance;
    s.messenger = vkbInstance.debug_messenger;
    volkLoadInstanceOnly(s.instance);
    s.caps.validationEnabled = validation && vkbInstance.debug_messenger != VK_NULL_HANDLE;
    if (validation && !s.caps.validationEnabled) {
        OX_LOG_WARN("rhi", "validation layers requested but VK_LAYER_KHRONOS_validation is not available");
    }
    s.caps.debugUtils = s.debugUtils;

    if (desc.surface) {
        s.surface = desc.surface->createSurface(s.instance);
        if (!s.surface) {
            error = "ISurfaceProvider::createSurface failed";
            return false;
        }
    }

    // ---------------- physical device ----------------
    VkPhysicalDeviceVulkan12Features req12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    req12.bufferDeviceAddress = VK_TRUE;
    req12.descriptorIndexing = VK_TRUE;
    req12.runtimeDescriptorArray = VK_TRUE;
    req12.descriptorBindingPartiallyBound = VK_TRUE;
    req12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    req12.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
    req12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    req12.timelineSemaphore = VK_TRUE;
    req12.scalarBlockLayout = VK_TRUE;

    vkb::PhysicalDeviceSelector selector(vkbInstance);
    selector.set_minimum_version(1, 2)
        .set_required_features_12(req12)
        .prefer_gpu_device_type(desc.preferDiscreteGpu ? vkb::PreferredDeviceType::discrete
                                                       : vkb::PreferredDeviceType::integrated)
        .allow_any_gpu_device_type(true);
    if (s.surface) {
        selector.set_surface(s.surface).require_present(true).add_required_extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }
    auto physRet = selector.select();
    if (!physRet) {
        error = "no suitable GPU (needs Vulkan 1.2 + descriptor indexing, BDA, timeline semaphores, scalar layout): " +
                physRet.error().message();
        return false;
    }
    vkb::PhysicalDevice vkbPhys = physRet.value();
    s.physical = vkbPhys.physical_device;

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(s.physical, &props);
    const u32 deviceApi = props.apiVersion;
    const bool core13 = (VK_API_VERSION_MINOR(deviceApi) >= 3 || VK_API_VERSION_MAJOR(deviceApi) > 1) &&
                        instanceApi >= VK_API_VERSION_1_3;
    const u32 useApi = core13 ? VK_API_VERSION_1_3 : VK_API_VERSION_1_2;

    u32 extCount = 0;
    vkEnumerateDeviceExtensionProperties(s.physical, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> exts(extCount);
    vkEnumerateDeviceExtensionProperties(s.physical, nullptr, &extCount, exts.data());

    std::vector<const char*> enableExts;
    auto want = [&](const char* name) {
        if (hasExtension(exts, name)) {
            enableExts.push_back(name);
            return true;
        }
        return false;
    };
    if (s.surface) enableExts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    s.caps.portabilitySubset = want("VK_KHR_portability_subset");
    if (!core13) {
        if (!want(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) || !want(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)) {
            error = "device lacks dynamic rendering / synchronization2";
            return false;
        }
        want(VK_KHR_MAINTENANCE_4_EXTENSION_NAME);
    }
    s.caps.memoryBudget = want(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    const bool hasDeferred = desc.enableRayTracing && want(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    const bool hasAS = hasDeferred && want(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    const bool hasRayQuery = hasAS && want(VK_KHR_RAY_QUERY_EXTENSION_NAME);
    const bool hasRtPipeline = hasAS && want(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
    const bool hasMesh = desc.enableMeshShaders && want(VK_EXT_MESH_SHADER_EXTENSION_NAME);
    const bool hasFsr = want(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);

    // ---------------- features: supported ∩ wanted ----------------
    VkPhysicalDeviceVulkan11Features sup11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features sup12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features sup13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceDynamicRenderingFeatures supDR{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
    VkPhysicalDeviceSynchronization2Features supS2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR supAS{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR supRQ{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR supRT{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    VkPhysicalDeviceMeshShaderFeaturesEXT supMesh{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR supFsr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
    VkPhysicalDeviceFeatures2 sup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    {
        void** next = &sup.pNext;
        auto chain = [&](auto& st) {
            *next = &st;
            next = &st.pNext;
        };
        chain(sup11);
        chain(sup12);
        if (core13) chain(sup13); else { chain(supDR); chain(supS2); }
        if (hasAS) chain(supAS);
        if (hasRayQuery) chain(supRQ);
        if (hasRtPipeline) chain(supRT);
        if (hasMesh) chain(supMesh);
        if (hasFsr) chain(supFsr);
        vkGetPhysicalDeviceFeatures2(s.physical, &sup);
    }
    if (!(core13 ? (sup13.dynamicRendering && sup13.synchronization2) : (supDR.dynamicRendering && supS2.synchronization2))) {
        error = "dynamicRendering / synchronization2 features missing";
        return false;
    }

    VkPhysicalDeviceFeatures en10{};
#define OX_OPT10(f) en10.f = sup.features.f
    OX_OPT10(samplerAnisotropy); OX_OPT10(shaderInt64); OX_OPT10(shaderInt16); OX_OPT10(shaderFloat64);
    OX_OPT10(fillModeNonSolid); OX_OPT10(wideLines); OX_OPT10(depthClamp); OX_OPT10(depthBiasClamp);
    OX_OPT10(independentBlend); OX_OPT10(multiDrawIndirect); OX_OPT10(drawIndirectFirstInstance);
    OX_OPT10(textureCompressionBC); OX_OPT10(textureCompressionASTC_LDR); OX_OPT10(textureCompressionETC2);
    OX_OPT10(sampleRateShading); OX_OPT10(geometryShader); OX_OPT10(tessellationShader); OX_OPT10(imageCubeArray);
    OX_OPT10(shaderStorageImageReadWithoutFormat); OX_OPT10(shaderStorageImageWriteWithoutFormat);
    OX_OPT10(fragmentStoresAndAtomics); OX_OPT10(vertexPipelineStoresAndAtomics); OX_OPT10(shaderImageGatherExtended);
    OX_OPT10(shaderStorageImageExtendedFormats); OX_OPT10(fullDrawIndexUint32); OX_OPT10(dualSrcBlend);
    OX_OPT10(depthBounds); OX_OPT10(shaderClipDistance); OX_OPT10(largePoints); OX_OPT10(multiViewport);
    OX_OPT10(pipelineStatisticsQuery); OX_OPT10(occlusionQueryPrecise);
#undef OX_OPT10

    VkPhysicalDeviceVulkan11Features en11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    en11.multiview = sup11.multiview;
    en11.shaderDrawParameters = sup11.shaderDrawParameters;
    en11.storageBuffer16BitAccess = sup11.storageBuffer16BitAccess;
    en11.uniformAndStorageBuffer16BitAccess = sup11.uniformAndStorageBuffer16BitAccess;

    VkPhysicalDeviceVulkan12Features en12 = req12;
    en12.pNext = nullptr;
#define OX_OPT12(f) en12.f = sup12.f
    OX_OPT12(drawIndirectCount); OX_OPT12(shaderOutputLayer); OX_OPT12(shaderOutputViewportIndex);
    OX_OPT12(shaderFloat16); OX_OPT12(shaderInt8); OX_OPT12(storageBuffer8BitAccess);
    OX_OPT12(uniformAndStorageBuffer8BitAccess); OX_OPT12(shaderStorageImageArrayNonUniformIndexing);
    OX_OPT12(shaderStorageBufferArrayNonUniformIndexing); OX_OPT12(shaderUniformBufferArrayNonUniformIndexing);
    OX_OPT12(descriptorBindingUpdateUnusedWhilePending); OX_OPT12(descriptorBindingVariableDescriptorCount);
    OX_OPT12(hostQueryReset); OX_OPT12(samplerFilterMinmax); OX_OPT12(vulkanMemoryModel);
    OX_OPT12(vulkanMemoryModelDeviceScope); OX_OPT12(shaderSubgroupExtendedTypes); OX_OPT12(separateDepthStencilLayouts);
    OX_OPT12(imagelessFramebuffer); OX_OPT12(uniformBufferStandardLayout); OX_OPT12(samplerMirrorClampToEdge);
    OX_OPT12(shaderBufferInt64Atomics); OX_OPT12(bufferDeviceAddressCaptureReplay);
#undef OX_OPT12

    VkPhysicalDeviceVulkan13Features en13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    en13.dynamicRendering = VK_TRUE;
    en13.synchronization2 = VK_TRUE;
    en13.maintenance4 = sup13.maintenance4;
    en13.shaderDemoteToHelperInvocation = sup13.shaderDemoteToHelperInvocation;
    en13.subgroupSizeControl = sup13.subgroupSizeControl;
    en13.computeFullSubgroups = sup13.computeFullSubgroups;
    en13.shaderTerminateInvocation = sup13.shaderTerminateInvocation;
    en13.shaderIntegerDotProduct = sup13.shaderIntegerDotProduct;
    VkPhysicalDeviceDynamicRenderingFeatures enDR{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
    enDR.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceSynchronization2Features enS2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
    enS2.synchronization2 = VK_TRUE;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR enAS{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    enAS.accelerationStructure = supAS.accelerationStructure;
    VkPhysicalDeviceRayQueryFeaturesKHR enRQ{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    enRQ.rayQuery = supRQ.rayQuery;
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR enRT{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    enRT.rayTracingPipeline = supRT.rayTracingPipeline;
    enRT.rayTraversalPrimitiveCulling = supRT.rayTraversalPrimitiveCulling;
    VkPhysicalDeviceMeshShaderFeaturesEXT enMesh{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    enMesh.meshShader = supMesh.meshShader;
    enMesh.taskShader = supMesh.taskShader;
    enMesh.multiviewMeshShader = supMesh.multiviewMeshShader && en11.multiview;
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR enFsr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
    enFsr.pipelineFragmentShadingRate = supFsr.pipelineFragmentShadingRate;
    enFsr.attachmentFragmentShadingRate = supFsr.attachmentFragmentShadingRate;
    enFsr.primitiveFragmentShadingRate = supFsr.primitiveFragmentShadingRate;
    if (en12.drawIndirectCount == VK_FALSE) {
        // VK_KHR_draw_indirect_count is promoted; fall back to the extension on older drivers.
        if (want(VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME)) s.caps.drawIndirectCount = true;
    }

    s.enabledFeatures = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    s.enabledFeatures.features = en10;
    {
        void** next = &s.enabledFeatures.pNext;
        auto chain = [&](auto& st) {
            *next = &st;
            next = &st.pNext;
        };
        chain(en11);
        chain(en12);
        if (core13) chain(en13); else { chain(enDR); chain(enS2); }
        if (hasAS && enAS.accelerationStructure) chain(enAS);
        if (hasRayQuery && enRQ.rayQuery) chain(enRQ);
        if (hasRtPipeline && enRT.rayTracingPipeline) chain(enRT);
        if (hasMesh && enMesh.meshShader) chain(enMesh);
        if (hasFsr && (enFsr.pipelineFragmentShadingRate || enFsr.attachmentFragmentShadingRate)) chain(enFsr);
    }

    // ---------------- queues ----------------
    u32 familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(s.physical, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(s.physical, &familyCount, families.data());
    auto presentOk = [&](u32 fam) {
        if (!s.surface) return true;
        VkBool32 ok = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(s.physical, fam, s.surface, &ok);
        return ok == VK_TRUE;
    };
    i32 gfx = -1;
    for (u32 i = 0; i < familyCount; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
            presentOk(i)) {
            gfx = i32(i);
            break;
        }
    }
    if (gfx < 0) {
        error = "no graphics+compute queue family with present support";
        return false;
    }
    for (u32 i = 0; i < familyCount; ++i) {
        OX_LOG_DEBUG("rhi", "queue family {}: flags 0x{:X}, {} queues, timestamp bits {}", i, families[i].queueFlags,
                     families[i].queueCount, families[i].timestampValidBits);
    }
    std::vector<u32> usedPerFamily(familyCount, 0);
    struct Slot { u32 family; u32 index; };
    std::array<Slot, kQueueTypeCount> slots{};
    slots[0] = {u32(gfx), 0};
    usedPerFamily[gfx] = 1;
    auto pickDedicated = [&](VkQueueFlags need, VkQueueFlags avoid) -> std::optional<Slot> {
        for (u32 i = 0; i < familyCount; ++i) {
            if ((families[i].queueFlags & need) == need && !(families[i].queueFlags & avoid) &&
                usedPerFamily[i] < families[i].queueCount) {
                return Slot{i, usedPerFamily[i]++};
            }
        }
        return std::nullopt;
    };
    auto extraInFamily = [&](u32 fam) -> std::optional<Slot> {
        if (usedPerFamily[fam] < families[fam].queueCount) return Slot{fam, usedPerFamily[fam]++};
        return std::nullopt;
    };
    std::optional<Slot> compute, transfer;
    if (desc.asyncCompute) {
        compute = pickDedicated(VK_QUEUE_COMPUTE_BIT, VK_QUEUE_GRAPHICS_BIT);
        if (!compute) compute = extraInFamily(u32(gfx));
        // e.g. MoltenVK: several identical G|C|T families with one queue each.
        if (!compute) compute = pickDedicated(VK_QUEUE_COMPUTE_BIT, 0);
    }
    if (desc.asyncTransfer) {
        transfer = pickDedicated(VK_QUEUE_TRANSFER_BIT, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT);
        if (!transfer) transfer = pickDedicated(VK_QUEUE_TRANSFER_BIT, VK_QUEUE_GRAPHICS_BIT);
        if (!transfer) transfer = extraInFamily(u32(gfx));
        if (!transfer) transfer = pickDedicated(VK_QUEUE_TRANSFER_BIT, 0);
    }
    slots[1] = compute.value_or(slots[0]);
    slots[2] = transfer.value_or(slots[0]);

    std::vector<std::vector<f32>> priorities(familyCount);
    for (u32 i = 0; i < familyCount; ++i) priorities[i].assign(usedPerFamily[i], 1.f);
    std::vector<VkDeviceQueueCreateInfo> queueInfos;
    for (u32 i = 0; i < familyCount; ++i) {
        if (usedPerFamily[i] == 0) continue;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = i;
        qi.queueCount = usedPerFamily[i];
        qi.pQueuePriorities = priorities[i].data();
        queueInfos.push_back(qi);
    }

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &s.enabledFeatures;
    dci.queueCreateInfoCount = u32(queueInfos.size());
    dci.pQueueCreateInfos = queueInfos.data();
    dci.enabledExtensionCount = u32(enableExts.size());
    dci.ppEnabledExtensionNames = enableExts.data();
    if (VkResult r = vkCreateDevice(s.physical, &dci, nullptr, &s.device); r != VK_SUCCESS) {
        error = std::format("vkCreateDevice: {}", vkResultName(r));
        return false;
    }
    volkLoadDevice(s.device);

    for (u32 t = 0; t < kQueueTypeCount; ++t) {
        i32 found = -1;
        for (u32 q = 0; q < s.queues.size(); ++q) {
            if (s.queues[q].family == slots[t].family && s.queues[q].index == slots[t].index) found = i32(q);
        }
        if (found < 0) {
            PhysicalQueue pq;
            pq.family = slots[t].family;
            pq.index = slots[t].index;
            vkGetDeviceQueue(s.device, pq.family, pq.index, &pq.queue);
            pq.timestamps = families[pq.family].timestampValidBits > 0;
            VkSemaphoreTypeCreateInfo ti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
            ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            ti.initialValue = 0;
            VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            si.pNext = &ti;
            OX_VK_CHECK(vkCreateSemaphore(s.device, &si, nullptr, &pq.timeline));
            s.queues.push_back(std::move(pq));
            found = i32(s.queues.size() - 1);
            setDebugName(VK_OBJECT_TYPE_SEMAPHORE, u64(s.queues.back().timeline),
                         std::format("timeline.{}", queueTypeName(QueueType(t))));
            setDebugName(VK_OBJECT_TYPE_QUEUE, u64(s.queues.back().queue), std::format("queue.{}", queueTypeName(QueueType(t))));
        }
        s.queueOf[t] = u32(found);
    }

    // ---------------- caps ----------------
    {
        DeviceCaps& c = s.caps;
        VkPhysicalDeviceVulkan11Properties p11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
        VkPhysicalDeviceVulkan12Properties p12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
        VkPhysicalDeviceAccelerationStructurePropertiesKHR pAS{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
        VkPhysicalDeviceRayTracingPipelinePropertiesKHR pRT{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &p11;
        p11.pNext = &p12;
        void** next = &p12.pNext;
        if (hasAS) { *next = &pAS; next = &pAS.pNext; }
        if (hasRtPipeline) { *next = &pRT; next = &pRT.pNext; }
        vkGetPhysicalDeviceProperties2(s.physical, &p2);
        const auto& lim = p2.properties.limits;

        c.gpuName = p2.properties.deviceName;
        c.driverName = p12.driverName;
        c.driverInfo = p12.driverInfo;
        c.vendorId = p2.properties.vendorID;
        c.deviceId = p2.properties.deviceID;
        c.vendor = gpuVendorFromId(c.vendorId);
        c.apiVersion = deviceApi;
        c.usedApiVersion = useApi;
        c.discreteGpu = p2.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;

        VkPhysicalDeviceMemoryProperties mem{};
        vkGetPhysicalDeviceMemoryProperties(s.physical, &mem);
        bool unified = true;
        for (u32 i = 0; i < mem.memoryHeapCount; ++i) {
            if (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) c.deviceLocalMemoryBytes += mem.memoryHeaps[i].size;
        }
        for (u32 i = 0; i < mem.memoryTypeCount; ++i) {
            const auto f = mem.memoryTypes[i].propertyFlags;
            if ((f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && !(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
                !(f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)) {
                unified = false;
            }
        }
        c.unifiedMemory = unified || c.vendor == GpuVendor::Apple;

        c.graphicsFamily = slots[0].family;
        c.computeFamily = slots[1].family;
        c.transferFamily = slots[2].family;
        c.asyncComputeQueue = s.queueOf[1] != s.queueOf[0];
        c.dedicatedTransferQueue = s.queueOf[2] != s.queueOf[0];

        c.accelerationStructure = hasAS && enAS.accelerationStructure;
        c.rayQuery = hasRayQuery && enRQ.rayQuery && c.accelerationStructure;
        c.rayTracingPipeline = hasRtPipeline && enRT.rayTracingPipeline && c.accelerationStructure;
        c.deferredHostOperations = hasDeferred;
        c.shaderGroupHandleSize = pRT.shaderGroupHandleSize;
        c.shaderGroupHandleAlignment = pRT.shaderGroupHandleAlignment;
        c.shaderGroupBaseAlignment = pRT.shaderGroupBaseAlignment;
        c.maxRayRecursionDepth = pRT.maxRayRecursionDepth;
        c.minAccelerationStructureScratchOffsetAlignment = pAS.minAccelerationStructureScratchOffsetAlignment;

        c.meshShader = hasMesh && enMesh.meshShader;
        c.taskShader = c.meshShader && enMesh.taskShader;
        c.geometryShader = en10.geometryShader;
        c.tessellationShader = en10.tessellationShader;
        c.multiview = en11.multiview;
        c.shaderOutputLayer = en12.shaderOutputLayer;
        c.shaderOutputViewportIndex = en12.shaderOutputViewportIndex;
        c.drawIndirectCount = c.drawIndirectCount || en12.drawIndirectCount;
        c.multiDrawIndirect = en10.multiDrawIndirect;
        c.fragmentShadingRate = hasFsr && (enFsr.pipelineFragmentShadingRate || enFsr.attachmentFragmentShadingRate);
        c.fillModeNonSolid = en10.fillModeNonSolid;
        c.wideLines = en10.wideLines;
        c.depthClamp = en10.depthClamp;
        c.independentBlend = en10.independentBlend;
        c.sampleRateShading = en10.sampleRateShading;
        c.shaderInt64 = en10.shaderInt64;
        c.shaderFloat64 = en10.shaderFloat64;
        c.shaderInt16 = en10.shaderInt16;
        c.shaderFloat16 = en12.shaderFloat16;
        c.storageImageReadWithoutFormat = en10.shaderStorageImageReadWithoutFormat;
        c.storageImageWriteWithoutFormat = en10.shaderStorageImageWriteWithoutFormat;

        c.samplerAnisotropy = en10.samplerAnisotropy;
        c.maxSamplerAnisotropy = lim.maxSamplerAnisotropy;
        c.textureCompressionBC = en10.textureCompressionBC;
        c.textureCompressionASTC = en10.textureCompressionASTC_LDR;
        c.textureCompressionETC2 = en10.textureCompressionETC2;
        c.maxImageDimension2D = lim.maxImageDimension2D;
        c.maxImageArrayLayers = lim.maxImageArrayLayers;
        c.maxColorAttachments = lim.maxColorAttachments;
        c.framebufferColorSampleCounts = lim.framebufferColorSampleCounts;
        c.framebufferDepthSampleCounts = lim.framebufferDepthSampleCounts;

        c.maxBindlessSampledImages = std::min(p12.maxDescriptorSetUpdateAfterBindSampledImages,
                                              p12.maxPerStageDescriptorUpdateAfterBindSampledImages);
        c.maxBindlessStorageImages = std::min(p12.maxDescriptorSetUpdateAfterBindStorageImages,
                                              p12.maxPerStageDescriptorUpdateAfterBindStorageImages);
        c.maxBindlessSamplers = std::min(p12.maxDescriptorSetUpdateAfterBindSamplers,
                                         p12.maxPerStageDescriptorUpdateAfterBindSamplers);
        c.maxPushConstantsSize = lim.maxPushConstantsSize;

        c.subgroupSize = p11.subgroupSize;
        c.subgroupSupportedStages = p11.subgroupSupportedStages;
        c.subgroupSupportedOperations = p11.subgroupSupportedOperations;
        c.maxComputeWorkGroupInvocations = lim.maxComputeWorkGroupInvocations;
        c.maxComputeSharedMemorySize = lim.maxComputeSharedMemorySize;

        c.hostQueryReset = en12.hostQueryReset;
        c.timestampQueries = (lim.timestampComputeAndGraphics || s.queue(QueueType::Graphics).timestamps) &&
                             lim.timestampPeriod > 0.f && c.hostQueryReset;
        c.timestampPeriodNs = lim.timestampPeriod;

        c.minUniformBufferOffsetAlignment = lim.minUniformBufferOffsetAlignment;
        c.minStorageBufferOffsetAlignment = lim.minStorageBufferOffsetAlignment;
        c.bufferImageGranularity = lim.bufferImageGranularity;
    }

    // Stages/access bits that are legal given the enabled features.
    s.supportedStages = ~0ull;
    if (!s.caps.rayTracingPipeline) s.supportedStages &= ~VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    if (!s.caps.accelerationStructure) s.supportedStages &= ~VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    if (!s.caps.meshShader) s.supportedStages &= ~(VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT);
    if (!s.caps.geometryShader) s.supportedStages &= ~VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT;
    if (!s.caps.tessellationShader) {
        s.supportedStages &= ~(VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT | VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT);
    }
    s.supportedAccess = ~0ull;
    if (!s.caps.accelerationStructure) {
        s.supportedAccess &= ~(VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
    }

    // ---------------- VMA ----------------
    {
        VmaVulkanFunctions funcs{};
        VmaAllocatorCreateInfo ai{};
        ai.physicalDevice = s.physical;
        ai.device = s.device;
        ai.instance = s.instance;
        ai.vulkanApiVersion = useApi;
        ai.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
        if (s.caps.memoryBudget) ai.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
        if (core13 && en13.maintenance4) ai.flags |= VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT;
        OX_VK_CHECK(vmaImportVulkanFunctionsFromVolk(&ai, &funcs));
        ai.pVulkanFunctions = &funcs;
        OX_VK_CHECK(vmaCreateAllocator(&ai, &s.allocator));
    }

    // ---------------- pipeline cache ----------------
    {
        std::vector<u8> data;
        if (!desc.pipelineCachePath.empty()) {
            data = readFile(desc.pipelineCachePath);
            // Header: length, version, vendorID, deviceID, UUID — drop caches from another GPU/driver.
            if (data.size() >= 32) {
                VkPhysicalDeviceProperties pp{};
                vkGetPhysicalDeviceProperties(s.physical, &pp);
                u32 vendor = 0, device = 0;
                std::memcpy(&vendor, data.data() + 8, 4);
                std::memcpy(&device, data.data() + 12, 4);
                if (vendor != pp.vendorID || device != pp.deviceID ||
                    std::memcmp(data.data() + 16, pp.pipelineCacheUUID, VK_UUID_SIZE) != 0) {
                    data.clear();
                }
            } else {
                data.clear();
            }
        }
        VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        ci.initialDataSize = data.size();
        ci.pInitialData = data.empty() ? nullptr : data.data();
        OX_VK_CHECK(vkCreatePipelineCache(s.device, &ci, nullptr, &s.pipelineCache));
        if (!data.empty()) OX_LOG_INFO("rhi", "pipeline cache loaded ({} KiB)", data.size() / 1024);
    }

    // ---------------- shader compiler ----------------
    {
        ShaderCompilerOptions so = desc.shaderOptions;
        if (so.cacheDirectory.empty()) {
            std::error_code ec;
            so.cacheDirectory = fs::temp_directory_path(ec) / "oxwald" / "shader_cache";
        }
        s.compiler = std::make_unique<ShaderCompiler>(std::move(so));
    }

    // ---------------- frames ----------------
    s.frames.resize(s.desc.framesInFlight);
    for (u32 i = 0; i < s.frames.size(); ++i) {
        FrameContext& f = s.frames[i];
        for (u32 q = 0; q < kQueueTypeCount; ++q) {
            VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            pi.queueFamilyIndex = s.queue(QueueType(q)).family;
            OX_VK_CHECK(vkCreateCommandPool(s.device, &pi, nullptr, &f.pools[q]));
        }
        if (s.caps.timestampQueries) {
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = 512;
            OX_VK_CHECK(vkCreateQueryPool(s.device, &qi, nullptr, &f.queryPool));
            vkResetQueryPool(s.device, f.queryPool, 0, qi.queryCount);
        }
    }
    for (u32 q = 0; q < kQueueTypeCount; ++q) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = s.queue(QueueType(q)).family;
        OX_VK_CHECK(vkCreateCommandPool(s.device, &pi, nullptr, &s.immediatePools[q]));
    }

    createBindless(s);
    createDefaultSamplers(*this, s);

    // Staging ring for asynchronous uploads.
    s.ringSize = std::max<u64>(desc.stagingRingSize, 1ull << 20);
    s.stagingRing = createBuffer({s.ringSize, BufferUsage::TransferSrc, MemoryUsage::Upload, "staging.ring"});

    s.renderDoc.initialize(false); // Tracy GPU context is created lazily in endFrame() once a profiler connects

    OX_LOG_INFO("rhi", "Device: {} | {} | Vulkan {}.{} | validation {} | frames in flight {}", s.caps.gpuName,
                s.caps.driverInfo, VK_API_VERSION_MAJOR(useApi), VK_API_VERSION_MINOR(useApi),
                s.caps.validationEnabled ? "on" : "off", s.desc.framesInFlight);
    return true;
}

Device::~Device() {
    DeviceState& s = *m_s;
    if (!s.device) {
        if (s.instance) {
            if (s.surface && s.surfaceProvider) s.surfaceProvider->destroySurface(s.instance, s.surface);
            if (s.messenger) vkDestroyDebugUtilsMessengerEXT(s.instance, s.messenger, nullptr);
            vkDestroyInstance(s.instance, nullptr);
        }
        return;
    }
    vkDeviceWaitIdle(s.device);
    savePipelineCache();
    destroyTracyContext(s);
    for (FrameContext& f : s.frames) {
        f.lists.clear();
        for (VkCommandPool p : f.pools) vkDestroyCommandPool(s.device, p, nullptr);
        if (f.queryPool) vkDestroyQueryPool(s.device, f.queryPool, nullptr);
    }
    s.frames.clear();
    s.collectGarbage(true);
    destroyAllResources(*this, s);
    s.collectGarbage(true);
    vkDestroyPipelineCache(s.device, s.pipelineCache, nullptr);
    vkDestroyPipelineLayout(s.device, s.pipelineLayout, nullptr);
    vkDestroyDescriptorPool(s.device, s.bindlessPool, nullptr);
    vkDestroyDescriptorSetLayout(s.device, s.bindlessLayout, nullptr);
    for (auto& q : s.queues) vkDestroySemaphore(s.device, q.timeline, nullptr);
    for (VkCommandPool p : s.immediatePools) vkDestroyCommandPool(s.device, p, nullptr);
    vmaDestroyAllocator(s.allocator);
    vkDestroyDevice(s.device, nullptr);
    if (s.surface && s.surfaceProvider) s.surfaceProvider->destroySurface(s.instance, s.surface);
    if (s.messenger) vkDestroyDebugUtilsMessengerEXT(s.instance, s.messenger, nullptr);
    vkDestroyInstance(s.instance, nullptr);
}

// ---------------------------------------------------------------------------------------------------------------
// accessors

const DeviceCaps& Device::caps() const { return m_s->caps; }
VkInstance Device::vkInstance() const { return m_s->instance; }
VkPhysicalDevice Device::vkPhysicalDevice() const { return m_s->physical; }
VkDevice Device::vkDevice() const { return m_s->device; }
VkQueue Device::vkQueue(QueueType q) const { return m_s->queues[m_s->queueOf[u32(q)]].queue; }
u32 Device::queueFamily(QueueType q) const { return m_s->queues[m_s->queueOf[u32(q)]].family; }
VkSurfaceKHR Device::surface() const { return m_s->surface; }
ISurfaceProvider* Device::surfaceProvider() const { return m_s->surfaceProvider; }
VkPipelineLayout Device::pipelineLayout() const { return m_s->pipelineLayout; }
VkDescriptorSet Device::bindlessSet() const { return m_s->bindlessSet; }
u64 Device::frameNumber() const { return m_s->frameNumber; }
u32 Device::frameIndex() const { return u32(m_s->frameNumber % m_s->frames.size()); }
u32 Device::framesInFlight() const { return u32(m_s->frames.size()); }
const std::vector<GpuTiming>& Device::gpuTimings() const { return m_s->lastTimings; }
void* Device::tracyGpuContext() const { return m_s->tracyCtx; }
RenderDocCapture& Device::renderDoc() { return m_s->renderDoc; }
std::string Device::lastPipelineError() const { return m_s->lastPipelineError; }
ShaderCompiler& Device::shaderCompiler() { return *m_s->compiler; }
VkSemaphore Device::timelineSemaphore(QueueType q) const { return m_s->queues[m_s->queueOf[u32(q)]].timeline; }

void Device::setDebugName(VkObjectType type, u64 handle, std::string_view name) {
    if (!m_s->debugUtils || !handle || name.empty() || !vkSetDebugUtilsObjectNameEXT) return;
    std::string n(name);
    VkDebugUtilsObjectNameInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = n.c_str();
    vkSetDebugUtilsObjectNameEXT(m_s->device, &info);
}

VkPipelineStageFlags2 DeviceState::sanitizeStages(VkPipelineStageFlags2 st, QueueType q) const {
    st &= supportedStages;
    if (q == QueueType::Transfer && queueOf[u32(QueueType::Transfer)] != queueOf[u32(QueueType::Graphics)]) {
        // Dedicated transfer queues only accept transfer/host stages.
        const VkPipelineStageFlags2 allowed = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT |
                                              VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT |
                                              VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        if (queues[queueOf[u32(QueueType::Transfer)]].family != queues[queueOf[u32(QueueType::Graphics)]].family) {
            st &= allowed;
        }
    }
    return st;
}

VkAccessFlags2 DeviceState::sanitizeAccess(VkAccessFlags2 a) const { return a & supportedAccess; }

// ---------------------------------------------------------------------------------------------------------------
// deferred destruction

void DeviceState::retire(std::function<void()> fn) {
    DeferredDeletion d;
    d.frame = inFrame ? i64(frameNumber) : -1;
    for (u32 i = 0; i < queues.size(); ++i) d.values[i] = queues[i].submitted;
    d.destroy = std::move(fn);
    std::lock_guard lock(deletionMutex);
    deletions.push_back(std::move(d));
}

void DeviceState::collectGarbage(bool all) {
    std::array<u64, kQueueTypeCount> completed{};
    if (!all) {
        for (u32 i = 0; i < queues.size(); ++i) vkGetSemaphoreCounterValue(device, queues[i].timeline, &completed[i]);
    }
    std::vector<std::function<void()>> ready;
    {
        std::lock_guard lock(deletionMutex);
        while (!deletions.empty()) {
            const DeferredDeletion& d = deletions.front();
            // An in-frame destroy may still be referenced by not-yet-submitted command lists of that frame,
            // so it also waits for the frame itself to complete.
            bool done = all || d.frame <= completedFrame;
            for (u32 i = 0; !all && done && i < queues.size(); ++i) done = completed[i] >= d.values[i];
            if (!done) break;
            ready.push_back(std::move(deletions.front().destroy));
            deletions.pop_front();
        }
    }
    for (auto& fn : ready) fn();
}

// ---------------------------------------------------------------------------------------------------------------
// frames & submission

namespace detail {
u32 reloadShadersImpl(Device& device, DeviceState& s, bool force);
}

void Device::beginFrame() {
    DeviceState& s = *m_s;
    if (s.inFrame) return;
    FrameContext& f = s.frames[frameIndex()];
    // Wait until the GPU finished the frame that last used this slot.
    std::vector<VkSemaphore> sems;
    std::vector<u64> values;
    for (u32 i = 0; i < s.queues.size(); ++i) {
        if (f.waitValues[i] > 0) {
            sems.push_back(s.queues[i].timeline);
            values.push_back(f.waitValues[i]);
        }
    }
    if (!sems.empty()) {
        VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wi.semaphoreCount = u32(sems.size());
        wi.pSemaphores = sems.data();
        wi.pValues = values.data();
        OX_VK_CHECK(vkWaitSemaphores(s.device, &wi, ~0ull));
    }
    s.completedFrame = i64(s.frameNumber) - i64(s.frames.size());
    readTimestamps(s, f);
    for (u32 q = 0; q < kQueueTypeCount; ++q) {
        OX_VK_CHECK(vkResetCommandPool(s.device, f.pools[q], 0));
        f.used[q] = 0;
    }
    f.lists.clear();
    f.frameNumber = s.frameNumber;
    s.inFrame = true;
    s.collectGarbage(false);
    if (s.desc.shaderHotReload) {
        reloadShadersImpl(*this, s, false);
    }
}

void Device::endFrame() {
    DeviceState& s = *m_s;
    if (!s.inFrame) beginFrame();
    flushUploads();
    collectTracy(*this, s);
    FrameContext& f = s.frames[frameIndex()];
    for (u32 i = 0; i < s.queues.size(); ++i) f.waitValues[i] = s.queues[i].submitted;
    ++s.frameNumber;
    s.inFrame = false;
}

CommandList& Device::commandList(QueueType queue, std::string_view debugName) {
    DeviceState& s = *m_s;
    if (!s.inFrame) beginFrame();
    FrameContext& f = s.frames[frameIndex()];
    const u32 q = u32(queue);
    if (f.used[q] == f.buffers[q].size()) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pools[q];
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        OX_VK_CHECK(vkAllocateCommandBuffers(s.device, &ai, &cb));
        f.buffers[q].push_back(cb);
    }
    VkCommandBuffer cb = f.buffers[q][f.used[q]++];
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    OX_VK_CHECK(vkBeginCommandBuffer(cb, &bi));
    if (!debugName.empty()) setDebugName(VK_OBJECT_TYPE_COMMAND_BUFFER, u64(cb), debugName);
    CommandList& list = f.lists.emplace_back(*this, cb, queue);
    list.m_frameSlot = i32(frameIndex());
    return list;
}

namespace {

void recordAcquires(Device& device, DeviceState& s, CommandList& cmd, const std::vector<PendingAcquire>& acquires) {
    std::vector<VkImageMemoryBarrier2> images;
    std::vector<VkBufferMemoryBarrier2> buffers;
    const u32 src = s.queue(QueueType::Transfer).family;
    const u32 dst = s.queue(cmd.queue()).family;
    for (const auto& a : acquires) {
        const AccessInfo info = accessInfo(a.dstAccess);
        if (a.texture) {
            const TextureRecord* t = s.textures.get(a.tex);
            if (!t) continue;
            VkImageMemoryBarrier2 b = makeImageBarrier(s, *t, 0, 0, info.stages, info.access, a.oldLayout, a.layout, cmd.queue());
            b.srcQueueFamilyIndex = src;
            b.dstQueueFamilyIndex = dst;
            images.push_back(b);
        } else {
            const BufferRecord* b = s.buffers.get(a.buffer);
            if (!b) continue;
            VkBufferMemoryBarrier2 bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            bb.dstStageMask = s.sanitizeStages(info.stages, cmd.queue());
            bb.dstAccessMask = s.sanitizeAccess(info.access);
            bb.srcQueueFamilyIndex = src;
            bb.dstQueueFamilyIndex = dst;
            bb.buffer = b->buffer;
            bb.size = VK_WHOLE_SIZE;
            buffers.push_back(bb);
        }
    }
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = u32(images.size());
    di.pImageMemoryBarriers = images.data();
    di.bufferMemoryBarrierCount = u32(buffers.size());
    di.pBufferMemoryBarriers = buffers.data();
    if (!images.empty() || !buffers.empty()) vkCmdPipelineBarrier2(cmd.vk(), &di);
    (void)device;
}

} // namespace

namespace detail {

u64 submitRaw(DeviceState& s, QueueType queue, std::span<const VkCommandBuffer> cmds, std::vector<VkSemaphoreSubmitInfo> waits,
              std::vector<VkSemaphoreSubmitInfo> signals) {
    PhysicalQueue& pq = s.queue(queue);
    std::lock_guard lock(*pq.mutex);
    const u64 value = ++pq.submitted;
    VkSemaphoreSubmitInfo tl{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    tl.semaphore = pq.timeline;
    tl.value = value;
    tl.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    signals.push_back(tl);
    std::vector<VkCommandBufferSubmitInfo> cbs;
    for (VkCommandBuffer cb : cmds) {
        VkCommandBufferSubmitInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        ci.commandBuffer = cb;
        cbs.push_back(ci);
    }
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = u32(waits.size());
    si.pWaitSemaphoreInfos = waits.data();
    si.commandBufferInfoCount = u32(cbs.size());
    si.pCommandBufferInfos = cbs.data();
    si.signalSemaphoreInfoCount = u32(signals.size());
    si.pSignalSemaphoreInfos = signals.data();
    OX_VK_CHECK(vkQueueSubmit2(pq.queue, 1, &si, VK_NULL_HANDLE));
    return value;
}

} // namespace detail

TimelinePoint Device::submit(CommandList& cmd, const SubmitInfo& info) {
    DeviceState& s = *m_s;
    OX_ASSERT(cmd.m_openTimestamps.empty(), "unbalanced beginTimestamp/endTimestamp in command list");
    if (s.uploadCmd && cmd.queue() != QueueType::Transfer) flushUploads(); // uploads must precede their consumers
    OX_VK_CHECK(vkEndCommandBuffer(cmd.vk()));
    const u32 self = s.queueOf[u32(cmd.queue())];

    std::vector<VkSemaphoreSubmitInfo> waits;
    auto addWait = [&](VkSemaphore sem, u64 value, VkPipelineStageFlags2 stages) {
        VkSemaphoreSubmitInfo w{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        w.semaphore = sem;
        w.value = value;
        w.stageMask = s.sanitizeStages(stages, cmd.queue());
        waits.push_back(w);
    };
    for (const TimelinePoint& tp : info.waits) {
        const u32 pq = s.queueOf[u32(tp.queue)];
        if (pq != self && tp.value > 0) addWait(s.queues[pq].timeline, tp.value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    }
    for (const SemaphoreWait& w : info.waitSemaphores) addWait(w.semaphore, w.value, w.stages);
    std::vector<VkSemaphoreSubmitInfo> signals;
    for (VkSemaphore sem : info.signalSemaphores) {
        VkSemaphoreSubmitInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        si.semaphore = sem;
        si.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        signals.push_back(si);
    }
    if (info.swapchain) {
        addWait(info.swapchain->acquireSemaphore(), 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        VkSemaphoreSubmitInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        si.semaphore = info.swapchain->presentSemaphore();
        si.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        signals.push_back(si);
    }

    std::vector<VkCommandBuffer> cmds;
    if (s.pendingUploadWait && cmd.queue() != QueueType::Transfer) {
        const u32 tq = s.queueOf[u32(QueueType::Transfer)];
        if (tq != self) addWait(s.queues[tq].timeline, s.pendingUploadWait->value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        if (cmd.queue() == QueueType::Graphics) {
            if (!s.pendingAcquires.empty()) {
                // Ownership acquire for async uploads, executed right before the user's commands.
                CommandList& acq = commandList(QueueType::Graphics, "upload.acquire");
                recordAcquires(*this, s, acq, s.pendingAcquires);
                OX_VK_CHECK(vkEndCommandBuffer(acq.vk()));
                cmds.push_back(acq.vk());
            }
            s.pendingAcquires.clear();
            s.pendingUploadWait.reset();
        }
    }
    cmds.push_back(cmd.vk());
    const u64 value = submitRaw(s, cmd.queue(), cmds, std::move(waits), std::move(signals));
    return {cmd.queue(), value};
}

void Device::immediateSubmit(const std::function<void(CommandList&)>& record, QueueType queue) {
    DeviceState& s = *m_s;
    std::lock_guard lock(s.immediateMutex);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = s.immediatePools[u32(queue)];
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    OX_VK_CHECK(vkAllocateCommandBuffers(s.device, &ai, &cb));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    OX_VK_CHECK(vkBeginCommandBuffer(cb, &bi));
    CommandList list(*this, cb, queue);
    record(list);
    OX_VK_CHECK(vkEndCommandBuffer(cb));
    std::vector<VkSemaphoreSubmitInfo> waits;
    // Make pending async uploads visible to immediate work too (no ownership acquire: tools/tests path).
    if (s.pendingUploadWait && queue != QueueType::Transfer &&
        s.queueOf[u32(QueueType::Transfer)] != s.queueOf[u32(queue)]) {
        VkSemaphoreSubmitInfo w{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        w.semaphore = s.queue(QueueType::Transfer).timeline;
        w.value = s.pendingUploadWait->value;
        w.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        waits.push_back(w);
    }
    const VkCommandBuffer cbs[] = {cb};
    const u64 value = submitRaw(s, queue, cbs, std::move(waits), {});
    wait({queue, value});
    vkFreeCommandBuffers(s.device, s.immediatePools[u32(queue)], 1, &cb);
}

void Device::wait(TimelinePoint point) {
    DeviceState& s = *m_s;
    if (point.value == 0) return;
    VkSemaphore sem = s.queues[s.queueOf[u32(point.queue)]].timeline;
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &sem;
    wi.pValues = &point.value;
    OX_VK_CHECK(vkWaitSemaphores(s.device, &wi, ~0ull));
}

bool Device::isComplete(TimelinePoint point) const {
    const DeviceState& s = *m_s;
    u64 v = 0;
    vkGetSemaphoreCounterValue(s.device, s.queues[s.queueOf[u32(point.queue)]].timeline, &v);
    return v >= point.value;
}

TimelinePoint Device::lastSubmitted(QueueType queue) const {
    return {queue, m_s->queues[m_s->queueOf[u32(queue)]].submitted};
}

void Device::waitIdle() {
    DeviceState& s = *m_s;
    vkDeviceWaitIdle(s.device);
    s.collectGarbage(true);
}

bool Device::savePipelineCache() {
    DeviceState& s = *m_s;
    if (s.desc.pipelineCachePath.empty() || !s.pipelineCache) return false;
    size_t size = 0;
    vkGetPipelineCacheData(s.device, s.pipelineCache, &size, nullptr);
    std::vector<u8> data(size);
    vkGetPipelineCacheData(s.device, s.pipelineCache, &size, data.data());
    std::error_code ec;
    fs::create_directories(s.desc.pipelineCachePath.parent_path(), ec);
    std::ofstream f(s.desc.pipelineCachePath, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(size));
    return bool(f);
}

GpuMemoryStats Device::memoryStats() const {
    const DeviceState& s = *m_s;
    GpuMemoryStats out;
    const VkPhysicalDeviceMemoryProperties* mp = nullptr;
    vmaGetMemoryProperties(s.allocator, &mp);
    std::vector<VmaBudget> budgets(mp->memoryHeapCount);
    vmaGetHeapBudgets(s.allocator, budgets.data());
    for (u32 i = 0; i < mp->memoryHeapCount; ++i) {
        GpuMemoryHeapStats h;
        h.budgetBytes = budgets[i].budget;
        h.usageBytes = budgets[i].usage;
        h.allocationBytes = budgets[i].statistics.allocationBytes;
        h.blockBytes = budgets[i].statistics.blockBytes;
        h.allocationCount = budgets[i].statistics.allocationCount;
        h.deviceLocal = (mp->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
        out.totalUsageBytes += h.usageBytes;
        out.totalBudgetBytes += h.budgetBytes;
        out.heaps.push_back(h);
    }
    out.bufferCount = u32(s.buffers.size());
    out.textureCount = u32(s.textures.size());
    return out;
}

} // namespace ox::rhi
