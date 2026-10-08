#include <oxwald/rhi/format.hpp>

#include <algorithm>

namespace ox::rhi {

const char* queueTypeName(QueueType q) {
    switch (q) {
    case QueueType::Graphics: return "graphics";
    case QueueType::Compute: return "compute";
    case QueueType::Transfer: return "transfer";
    }
    return "?";
}

u32 fullMipCount(u32 width, u32 height, u32 depth) {
    u32 m = std::max({width, height, depth, 1u});
    u32 n = 1;
    while (m > 1) {
        m >>= 1;
        ++n;
    }
    return n;
}

FormatInfo formatInfo(VkFormat f) {
    FormatInfo i;
    auto plain = [&](u32 bytes, bool srgb = false) {
        i.blockBytes = bytes;
        i.srgb = srgb;
    };
    auto block = [&](u32 bytes, u32 w, u32 h, bool srgb = false) {
        i.blockBytes = bytes;
        i.blockWidth = w;
        i.blockHeight = h;
        i.compressed = true;
        i.srgb = srgb;
    };
    switch (f) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: plain(1); break;
    case VK_FORMAT_R8_SRGB: plain(1, true); break;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT: plain(2); break;
    case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SNORM: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT:
    case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_B5G6R5_UNORM_PACK16:
    case VK_FORMAT_R4G4B4A4_UNORM_PACK16: case VK_FORMAT_A1R5G5B5_UNORM_PACK16: plain(2); break;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SNORM: case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32: case VK_FORMAT_A2B10G10R10_UINT_PACK32:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
    case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_SNORM: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT:
    case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32_SFLOAT: plain(4); break;
    case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_SRGB: plain(4, true); break;
    case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM: case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32_SFLOAT: plain(8); break;
    case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32_SINT: case VK_FORMAT_R32G32B32_SFLOAT: plain(12); break;
    case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: case VK_FORMAT_R32G32B32A32_SFLOAT: plain(16); break;

    case VK_FORMAT_D16_UNORM: i.blockBytes = 2; i.depth = true; break;
    case VK_FORMAT_X8_D24_UNORM_PACK32: i.blockBytes = 4; i.depth = true; break;
    case VK_FORMAT_D32_SFLOAT: i.blockBytes = 4; i.depth = true; break;
    case VK_FORMAT_S8_UINT: i.blockBytes = 1; i.stencil = true; break;
    case VK_FORMAT_D16_UNORM_S8_UINT: i.blockBytes = 3; i.depth = i.stencil = true; break;
    case VK_FORMAT_D24_UNORM_S8_UINT: i.blockBytes = 4; i.depth = i.stencil = true; break;
    case VK_FORMAT_D32_SFLOAT_S8_UINT: i.blockBytes = 5; i.depth = i.stencil = true; break;

    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC4_UNORM_BLOCK:
    case VK_FORMAT_BC4_SNORM_BLOCK: block(8, 4, 4); break;
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: block(8, 4, 4, true); break;
    case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC5_SNORM_BLOCK: case VK_FORMAT_BC6H_UFLOAT_BLOCK: case VK_FORMAT_BC6H_SFLOAT_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK: block(16, 4, 4); break;
    case VK_FORMAT_BC2_SRGB_BLOCK: case VK_FORMAT_BC3_SRGB_BLOCK: case VK_FORMAT_BC7_SRGB_BLOCK: block(16, 4, 4, true); break;
    case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK: case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK: case VK_FORMAT_EAC_R11_UNORM_BLOCK:
    case VK_FORMAT_EAC_R11_SNORM_BLOCK: block(8, 4, 4); break;
    case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK: case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK: block(8, 4, 4, true); break;
    case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK: case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
    case VK_FORMAT_EAC_R11G11_SNORM_BLOCK: block(16, 4, 4); break;
    case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK: block(16, 4, 4, true); break;
    case VK_FORMAT_ASTC_4x4_UNORM_BLOCK: block(16, 4, 4); break;
    case VK_FORMAT_ASTC_4x4_SRGB_BLOCK: block(16, 4, 4, true); break;
    case VK_FORMAT_ASTC_6x6_UNORM_BLOCK: block(16, 6, 6); break;
    case VK_FORMAT_ASTC_6x6_SRGB_BLOCK: block(16, 6, 6, true); break;
    case VK_FORMAT_ASTC_8x8_UNORM_BLOCK: block(16, 8, 8); break;
    case VK_FORMAT_ASTC_8x8_SRGB_BLOCK: block(16, 8, 8, true); break;
    default: plain(4); break; // conservative fallback for estimates
    }
    return i;
}

VkImageAspectFlags formatAspect(VkFormat format) {
    const FormatInfo i = formatInfo(format);
    VkImageAspectFlags a = 0;
    if (i.depth) a |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (i.stencil) a |= VK_IMAGE_ASPECT_STENCIL_BIT;
    return a ? a : VK_IMAGE_ASPECT_COLOR_BIT;
}

std::string_view formatName(VkFormat f) {
    switch (f) {
#define OX_F(x) case VK_FORMAT_##x: return #x;
        OX_F(UNDEFINED) OX_F(R8_UNORM) OX_F(R8G8_UNORM) OX_F(R8G8B8A8_UNORM) OX_F(R8G8B8A8_SRGB) OX_F(B8G8R8A8_UNORM)
        OX_F(B8G8R8A8_SRGB) OX_F(A2B10G10R10_UNORM_PACK32) OX_F(A2R10G10B10_UNORM_PACK32) OX_F(B10G11R11_UFLOAT_PACK32)
        OX_F(R16_SFLOAT) OX_F(R16G16_SFLOAT) OX_F(R16G16B16A16_SFLOAT) OX_F(R32_SFLOAT) OX_F(R32_UINT) OX_F(R32G32_SFLOAT)
        OX_F(R32G32B32A32_SFLOAT) OX_F(D16_UNORM) OX_F(D32_SFLOAT) OX_F(D24_UNORM_S8_UINT) OX_F(D32_SFLOAT_S8_UINT)
        OX_F(BC1_RGBA_UNORM_BLOCK) OX_F(BC3_UNORM_BLOCK) OX_F(BC5_UNORM_BLOCK) OX_F(BC7_UNORM_BLOCK) OX_F(BC7_SRGB_BLOCK)
        OX_F(ASTC_4x4_UNORM_BLOCK) OX_F(ETC2_R8G8B8A8_UNORM_BLOCK)
#undef OX_F
    default: return "VkFormat(other)";
    }
}

u64 mipLevelSize(VkFormat format, u32 width, u32 height, u32 depth, u32 mip) {
    const FormatInfo i = formatInfo(format);
    const u32 w = std::max(1u, width >> mip);
    const u32 h = std::max(1u, height >> mip);
    const u32 d = std::max(1u, depth >> mip);
    const u64 bw = (w + i.blockWidth - 1) / i.blockWidth;
    const u64 bh = (h + i.blockHeight - 1) / i.blockHeight;
    return bw * bh * d * i.blockBytes;
}

u64 estimateTextureSize(const TextureDesc& desc) {
    const u32 mips = desc.mipLevels == 0 ? fullMipCount(desc.width, desc.height, desc.depth) : desc.mipLevels;
    u64 total = 0;
    for (u32 m = 0; m < mips; ++m) {
        total += mipLevelSize(desc.format, desc.width, desc.height, desc.depth, m);
    }
    total *= std::max(1u, desc.arrayLayers) * std::max(1u, desc.samples);
    return (total + 0xFFFF) & ~u64(0xFFFF); // 64 KiB granularity, like typical GPU page sizes
}

VkImageViewType defaultViewType(TextureType type, u32 layerCount) {
    switch (type) {
    case TextureType::Tex1D: return layerCount > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
    case TextureType::Tex2D: return layerCount > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    case TextureType::Tex3D: return VK_IMAGE_VIEW_TYPE_3D;
    case TextureType::Cube: return layerCount > 6 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
    }
    return VK_IMAGE_VIEW_TYPE_2D;
}

} // namespace ox::rhi
