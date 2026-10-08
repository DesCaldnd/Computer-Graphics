#include <oxwald/rhi/device_caps.hpp>
#include <oxwald/rhi/vulkan.hpp>

#include <format>

namespace ox::rhi {

const char* gpuVendorName(GpuVendor v) {
    switch (v) {
    case GpuVendor::Nvidia: return "NVIDIA";
    case GpuVendor::Amd: return "AMD";
    case GpuVendor::Intel: return "Intel";
    case GpuVendor::Apple: return "Apple";
    case GpuVendor::Arm: return "ARM";
    case GpuVendor::Qualcomm: return "Qualcomm";
    case GpuVendor::ImgTec: return "Imagination";
    case GpuVendor::Microsoft: return "Microsoft";
    case GpuVendor::Unknown: break;
    }
    return "Unknown";
}

GpuVendor gpuVendorFromId(u32 id) {
    switch (id) {
    case 0x10DE: return GpuVendor::Nvidia;
    case 0x1002: return GpuVendor::Amd;
    case 0x8086: return GpuVendor::Intel;
    case 0x106B: return GpuVendor::Apple;
    case 0x13B5: return GpuVendor::Arm;
    case 0x5143: return GpuVendor::Qualcomm;
    case 0x1010: return GpuVendor::ImgTec;
    case 0x1414: return GpuVendor::Microsoft;
    default: return GpuVendor::Unknown;
    }
}

std::string DeviceCaps::whyRayTracingUnavailable() const {
    if (rayTracingSupported()) {
        return {};
    }
    std::string missing;
    if (!accelerationStructure) missing += "VK_KHR_acceleration_structure";
    if (!rayQuery) missing += missing.empty() ? "VK_KHR_ray_query" : ", VK_KHR_ray_query";
    std::string reason = std::format("{} does not expose {}", gpuName.empty() ? "The GPU" : gpuName, missing);
    if (portabilitySubset || vendor == GpuVendor::Apple) {
        reason += " (MoltenVK translates Vulkan to Metal and does not implement Vulkan ray tracing yet)";
    } else {
        reason += " (requires a ray tracing capable GPU and an up-to-date driver)";
    }
    return reason;
}

std::string DeviceCaps::toString() const {
    auto yn = [](bool b) { return b ? "yes" : "no"; };
    std::string s;
    s += std::format("GPU: {} ({}, vendor 0x{:04X}, device 0x{:04X}){}\n", gpuName, gpuVendorName(vendor), vendorId,
                     deviceId, discreteGpu ? " discrete" : "");
    s += std::format("Driver: {} {}\n", driverName, driverInfo);
    s += std::format("Vulkan: device {}.{}.{}, using {}.{}; portability subset: {}; unified memory: {}; VRAM {} MiB\n",
                     VK_API_VERSION_MAJOR(apiVersion), VK_API_VERSION_MINOR(apiVersion), VK_API_VERSION_PATCH(apiVersion),
                     VK_API_VERSION_MAJOR(usedApiVersion), VK_API_VERSION_MINOR(usedApiVersion), yn(portabilitySubset),
                     yn(unifiedMemory), deviceLocalMemoryBytes >> 20);
    s += std::format("Queues: graphics family {}, async compute {} (family {}), dedicated transfer {} (family {})\n",
                     graphicsFamily, yn(asyncComputeQueue), computeFamily, yn(dedicatedTransferQueue), transferFamily);
    s += std::format("Ray tracing: AS {}, ray query {}, RT pipeline {}, deferred host ops {}{}\n", yn(accelerationStructure),
                     yn(rayQuery), yn(rayTracingPipeline), yn(deferredHostOperations),
                     rayTracingSupported() ? "" : std::format(" -> unavailable: {}", whyRayTracingUnavailable()));
    s += std::format("Geometry: mesh {}, task {}, geometry shader {}, tessellation {}, multiview {}, VS layer output {}, "
                     "viewport index {}, drawIndirectCount {}, multiDrawIndirect {}, fragment shading rate {}\n",
                     yn(meshShader), yn(taskShader), yn(geometryShader), yn(tessellationShader), yn(multiview),
                     yn(shaderOutputLayer), yn(shaderOutputViewportIndex), yn(drawIndirectCount), yn(multiDrawIndirect),
                     yn(fragmentShadingRate));
    s += std::format("Shader types: int64 {}, float64 {}, int16 {}, float16 {}; storage image r/w without format {}/{}\n",
                     yn(shaderInt64), yn(shaderFloat64), yn(shaderInt16), yn(shaderFloat16),
                     yn(storageImageReadWithoutFormat), yn(storageImageWriteWithoutFormat));
    s += std::format("Textures: anisotropy {} (max {}), BC {}, ASTC {}, ETC2 {}, max 2D {}, max layers {}, color samples 0x{:X}\n",
                     yn(samplerAnisotropy), maxSamplerAnisotropy, yn(textureCompressionBC), yn(textureCompressionASTC),
                     yn(textureCompressionETC2), maxImageDimension2D, maxImageArrayLayers, framebufferColorSampleCounts);
    s += std::format("Bindless: sampled images {}, storage images {}, samplers {}, push constants {} B\n",
                     maxBindlessSampledImages, maxBindlessStorageImages, maxBindlessSamplers, maxPushConstantsSize);
    s += std::format("Subgroups: size {}, stages 0x{:X}, ops 0x{:X}; compute max invocations {}, shared mem {} KiB\n",
                     subgroupSize, subgroupSupportedStages, subgroupSupportedOperations, maxComputeWorkGroupInvocations,
                     maxComputeSharedMemorySize / 1024);
    s += std::format("Timestamps: {} (period {} ns); memory budget {}; debug utils {}; validation {}",
                     yn(timestampQueries), timestampPeriodNs, yn(memoryBudget), yn(debugUtils), yn(validationEnabled));
    return s;
}

} // namespace ox::rhi
