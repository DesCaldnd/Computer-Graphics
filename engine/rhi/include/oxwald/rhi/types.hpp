#pragma once

#include <oxwald/core/types.hpp>
#include <oxwald/rhi/handles.hpp>
#include <oxwald/rhi/vulkan.hpp>

#include <array>
#include <string>
#include <type_traits>

namespace ox::rhi {

#define OX_RHI_FLAGS(E)                                                                                     \
    constexpr E operator|(E a, E b) { return E(std::underlying_type_t<E>(a) | std::underlying_type_t<E>(b)); } \
    constexpr E operator&(E a, E b) { return E(std::underlying_type_t<E>(a) & std::underlying_type_t<E>(b)); } \
    constexpr E& operator|=(E& a, E b) { return a = a | b; }                                                  \
    constexpr bool any(E a) { return std::underlying_type_t<E>(a) != 0; }

enum class QueueType : u8 { Graphics = 0, Compute = 1, Transfer = 2 };
inline constexpr u32 kQueueTypeCount = 3;
const char* queueTypeName(QueueType q);

// A point on a queue's timeline semaphore. Every submission signals the next value.
struct TimelinePoint {
    QueueType queue = QueueType::Graphics;
    u64 value = 0;
};

enum class MemoryUsage : u8 {
    GpuOnly,  // device local, not mappable
    Upload,   // host visible, sequential CPU writes (staging, per-frame constants)
    Readback, // host visible + cached, CPU reads GPU results
    Dynamic,  // host visible, prefer device local (UMA / ReBAR) — written by CPU each frame, read by GPU
};

enum class BufferUsage : u32 {
    None = 0,
    Vertex = 1u << 0,
    Index = 1u << 1,
    Uniform = 1u << 2,
    Storage = 1u << 3,
    Indirect = 1u << 4,
    TransferSrc = 1u << 5,
    TransferDst = 1u << 6,
    AccelStructInput = 1u << 7,   // vertex/index/instance data for AS builds
    AccelStructStorage = 1u << 8, // backing memory of an acceleration structure
    ShaderBindingTable = 1u << 9,
};
OX_RHI_FLAGS(BufferUsage)

struct BufferDesc {
    u64 size = 0;
    BufferUsage usage = BufferUsage::Storage;
    MemoryUsage memory = MemoryUsage::GpuOnly;
    std::string name;
};

enum class TextureType : u8 { Tex1D, Tex2D, Tex3D, Cube };

enum class TextureUsage : u32 {
    None = 0,
    Sampled = 1u << 0,
    Storage = 1u << 1,
    ColorAttachment = 1u << 2,
    DepthStencilAttachment = 1u << 3,
    TransferSrc = 1u << 4,
    TransferDst = 1u << 5,
};
OX_RHI_FLAGS(TextureUsage)

struct TextureDesc {
    TextureType type = TextureType::Tex2D;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    u32 width = 1;
    u32 height = 1;
    u32 depth = 1;
    u32 mipLevels = 1;   // 0 = full chain
    u32 arrayLayers = 1; // for Cube: number of faces (multiple of 6)
    u32 samples = 1;
    TextureUsage usage = TextureUsage::Sampled | TextureUsage::TransferDst;
    std::string name;

    bool operator==(const TextureDesc&) const = default;
};

u32 fullMipCount(u32 width, u32 height, u32 depth = 1);

struct TextureSubresource {
    u32 baseMip = 0;
    u32 mipCount = ~0u;   // ~0u = remaining
    u32 baseLayer = 0;
    u32 layerCount = ~0u; // ~0u = remaining
    bool operator==(const TextureSubresource&) const = default;
};

struct TextureViewDesc {
    TextureSubresource range;
    VkImageViewType type = VK_IMAGE_VIEW_TYPE_MAX_ENUM; // MAX_ENUM = derive from texture + range
    VkFormat format = VK_FORMAT_UNDEFINED;              // UNDEFINED = texture format
    bool operator==(const TextureViewDesc&) const = default;
};

struct SamplerDesc {
    VkFilter magFilter = VK_FILTER_LINEAR;
    VkFilter minFilter = VK_FILTER_LINEAR;
    VkSamplerMipmapMode mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    VkSamplerAddressMode addressU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSamplerAddressMode addressV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSamplerAddressMode addressW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    f32 mipLodBias = 0.f;
    f32 maxAnisotropy = 0.f; // 0 = off; clamped to device limit
    bool compareEnable = false;
    VkCompareOp compareOp = VK_COMPARE_OP_GREATER_OR_EQUAL; // reversed-Z shadow comparison
    f32 minLod = 0.f;
    f32 maxLod = VK_LOD_CLAMP_NONE;
    VkBorderColor borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    bool operator==(const SamplerDesc&) const = default;
};

// Samplers created by the device at startup; their bindless indices are fixed so shaders can use constants
// (mirrored in engine/shaders/common/bindless.glsl).
enum class DefaultSampler : u32 {
    LinearRepeat = 0,
    LinearClamp = 1,
    NearestRepeat = 2,
    NearestClamp = 3,
    AnisotropicRepeat = 4,
    ShadowCompare = 5, // linear, clamp to border, compare GREATER_OR_EQUAL
    Count
};

struct MemoryRequirements {
    u64 size = 0;
    u64 alignment = 1;
    u32 memoryTypeBits = ~0u;
};

struct ClearColor {
    std::array<f32, 4> f{0.f, 0.f, 0.f, 0.f};
    static ClearColor rgba(f32 r, f32 g, f32 b, f32 a = 1.f) { return {{r, g, b, a}}; }
};

struct ClearDepthStencil {
    f32 depth = 0.f; // reversed-Z: far = 0
    u32 stencil = 0;
};

} // namespace ox::rhi
