#pragma once

// GPU structures of the volumetrics passes. Mirrors engine/shaders/render/volumetrics/volumetrics_types.glsl
// (scalar layout): keep both sides in sync.

#include <oxwald/core/math.hpp>
#include <oxwald/render/gpu_types.hpp>

namespace ox::render::volumetrics {

enum FogFlags : u32 {
    kFogSunShadows = 1u << 0,
    kFogLocalShadows = 1u << 1,
    kFogCloudShadows = 1u << 2,
    kFogVisibility = 1u << 3,
    kFogJitter = 1u << 4,
};

struct GpuFogVolume {
    glm::vec4 unitRows[3]{};       // world → unit shape space (affine rows)
    glm::vec4 albedoDensity{0.0f}; // rgb albedo, a extinction (1/m)
    glm::vec4 emissionG{0.0f};     // rgb emission (display-relative), a anisotropy
    glm::vec4 params{0.0f};        // falloff, noise intensity, noise frequency, shape
    glm::vec4 noiseOffset{0.0f};
};
static_assert(sizeof(GpuFogVolume) == 112);

struct GpuFogConstants {
    glm::uvec4 grid{0u};
    glm::vec4 jitter{0.0f};
    glm::vec4 prevCamera{0.0f};
    glm::vec4 prevForward{0.0f};
    glm::vec4 emission{0.0f};
    glm::vec4 wind{0.0f};
    glm::vec4 cloudLayer{0.0f};
    glm::vec4 cloudWeather{0.0f};
    u64 volumes = 0;
    u32 noiseTexture = kInvalidIndex;
    u32 weatherTexture = kInvalidIndex;
    u32 visibilityTexture = kInvalidIndex;
    u32 flags = 0;
    u32 pad0 = 0, pad1 = 0;
};
static_assert(sizeof(GpuFogConstants) == 160);

struct GpuCloudConstants {
    glm::vec4 layer{0.0f};
    glm::vec4 shape{0.0f};
    glm::vec4 wind{0.0f};
    glm::vec4 weather{0.0f};
    glm::vec4 lighting{0.0f};
    glm::vec4 albedo{0.0f};
    glm::uvec4 size{0u};
    glm::uvec4 march{0u};
    glm::uvec4 misc{0u};
    u32 baseNoise = kInvalidIndex;
    u32 detailNoise = kInvalidIndex;
    u32 weatherTexture = kInvalidIndex;
    u32 depthTexture = kInvalidIndex;
};
static_assert(sizeof(GpuCloudConstants) == 160);

} // namespace ox::render::volumetrics
