#pragma once

#include <oxwald/core/types.hpp>

#include <string>

namespace ox::rhi {

enum class GpuVendor : u8 { Unknown, Nvidia, Amd, Intel, Apple, Arm, Qualcomm, ImgTec, Microsoft };

// Everything the renderer needs to know about the selected GPU. Filled once at device creation.
struct DeviceCaps {
    // identity
    std::string gpuName;
    std::string driverName;
    std::string driverInfo;
    GpuVendor vendor = GpuVendor::Unknown;
    u32 vendorId = 0;
    u32 deviceId = 0;
    u32 apiVersion = 0;    // device-reported Vulkan version
    u32 usedApiVersion = 0; // version the device was created with
    bool discreteGpu = false;
    bool unifiedMemory = false; // every device-local heap is host visible (Apple, integrated)
    bool portabilitySubset = false; // MoltenVK & co (VK_KHR_portability_subset)

    // queues
    bool asyncComputeQueue = false;  // separate VkQueue for compute
    bool dedicatedTransferQueue = false;
    u32 graphicsFamily = 0, computeFamily = 0, transferFamily = 0;

    // ray tracing
    bool accelerationStructure = false;
    bool rayQuery = false;
    bool rayTracingPipeline = false;
    bool deferredHostOperations = false;
    u32 shaderGroupHandleSize = 0;
    u32 shaderGroupHandleAlignment = 0;
    u32 shaderGroupBaseAlignment = 0;
    u32 maxRayRecursionDepth = 0;
    u32 minAccelerationStructureScratchOffsetAlignment = 0;

    // geometry pipeline
    bool meshShader = false;
    bool taskShader = false;
    bool geometryShader = false;
    bool tessellationShader = false;
    bool multiview = false;
    bool shaderOutputLayer = false;    // gl_Layer from vertex shaders (layered rendering without GS)
    bool shaderOutputViewportIndex = false;
    bool drawIndirectCount = false;
    bool multiDrawIndirect = false;
    bool fragmentShadingRate = false;
    bool fillModeNonSolid = false;
    bool wideLines = false;
    bool depthClamp = false;
    bool independentBlend = false;
    bool sampleRateShading = false;
    bool shaderInt64 = false;
    bool shaderFloat64 = false;
    bool shaderInt16 = false;
    bool shaderFloat16 = false;
    bool storageImageReadWithoutFormat = false;
    bool storageImageWriteWithoutFormat = false;

    // textures
    bool samplerAnisotropy = false;
    f32 maxSamplerAnisotropy = 1.f;
    bool textureCompressionBC = false;
    bool textureCompressionASTC = false;
    bool textureCompressionETC2 = false;
    u32 maxImageDimension2D = 0;
    u32 maxImageArrayLayers = 0;
    u32 maxColorAttachments = 0;
    u32 framebufferColorSampleCounts = 0;
    u32 framebufferDepthSampleCounts = 0;

    // bindless
    u32 maxBindlessSampledImages = 0;
    u32 maxBindlessStorageImages = 0;
    u32 maxBindlessSamplers = 0;
    u32 maxPushConstantsSize = 0;

    // compute & subgroups
    u32 subgroupSize = 0;
    u32 subgroupSupportedStages = 0;     // VkShaderStageFlags
    u32 subgroupSupportedOperations = 0; // VkSubgroupFeatureFlags
    u32 maxComputeWorkGroupInvocations = 0;
    u32 maxComputeSharedMemorySize = 0;

    // queries / profiling
    bool timestampQueries = false;
    f32 timestampPeriodNs = 0.f;
    bool memoryBudget = false;
    bool debugUtils = false;
    bool validationEnabled = false;
    bool hostQueryReset = false;

    // misc limits
    u64 minUniformBufferOffsetAlignment = 0;
    u64 minStorageBufferOffsetAlignment = 0;
    u64 bufferImageGranularity = 0;
    u64 deviceLocalMemoryBytes = 0;

    [[nodiscard]] bool rayTracingSupported() const { return accelerationStructure && rayQuery; }
    // Empty when ray tracing is available; otherwise a human readable reason for the UI tooltip.
    [[nodiscard]] std::string whyRayTracingUnavailable() const;
    [[nodiscard]] std::string toString() const;
};

const char* gpuVendorName(GpuVendor v);
GpuVendor gpuVendorFromId(u32 vendorId);

} // namespace ox::rhi
