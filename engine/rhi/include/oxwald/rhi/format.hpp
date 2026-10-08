#pragma once

#include <oxwald/rhi/types.hpp>

#include <string_view>

namespace ox::rhi {

struct FormatInfo {
    u32 blockBytes = 0;  // bytes per texel, or per block for compressed formats
    u32 blockWidth = 1;  // texels per block (compressed formats)
    u32 blockHeight = 1;
    bool depth = false;
    bool stencil = false;
    bool compressed = false;
    bool srgb = false;
};

FormatInfo formatInfo(VkFormat format);
VkImageAspectFlags formatAspect(VkFormat format);
std::string_view formatName(VkFormat format);

// Bytes of one tightly packed mip level (all layers excluded).
u64 mipLevelSize(VkFormat format, u32 width, u32 height, u32 depth, u32 mip);
// Estimated memory footprint (sum over mips × layers × samples), used by the render graph planner
// when real memory requirements are unavailable (CPU-only tests).
u64 estimateTextureSize(const TextureDesc& desc);

VkImageViewType defaultViewType(TextureType type, u32 layerCount);

} // namespace ox::rhi
