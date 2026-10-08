#pragma once

// GPU-ready texture data (.oxtex). Mips are stored separately so a streaming GPU cache can read the small
// tail first: readTextureInfo() parses only the header + mip table, readMipRange() fetches selected levels.

#include <oxwald/assets/asset_types.hpp>
#include <oxwald/core/result.hpp>

#include <filesystem>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

namespace ox::assets {

// Values are part of the .oxtex format: append only.
enum class TextureFormat : u16 {
    Unknown = 0,
    R8Unorm = 1,
    RG8Unorm = 2,
    RGBA8Unorm = 3,
    RGBA8Srgb = 4,
    RGBA16Float = 5,
    RGBA32Float = 6,
    BC5Unorm = 7,   // RG normal maps (Z reconstructed in the shader)
    BC7Unorm = 8,
    BC7Srgb = 9,
    BC6HUfloat = 10, // reserved: no encoder available, HDR is stored as RGBA16Float
    R16Unorm = 11,
    R32Float = 12,
};

[[nodiscard]] std::string_view textureFormatName(TextureFormat f);
// VkFormat enum value (e.g. VK_FORMAT_BC7_SRGB_BLOCK = 146); 0 for Unknown.
[[nodiscard]] u32 toVkFormat(TextureFormat f);
[[nodiscard]] bool isBlockCompressed(TextureFormat f);
[[nodiscard]] bool isSrgb(TextureFormat f);
// Bytes per texel, or per 4x4 block for block-compressed formats.
[[nodiscard]] u32 formatBlockBytes(TextureFormat f);
// Size of one face/layer of a w×h level (block formats round up to whole 4x4 blocks).
[[nodiscard]] u64 textureLevelSize(TextureFormat f, u32 width, u32 height);

enum class TextureWrap : u8 { Repeat, Clamp, Mirror };
enum class TextureFilter : u8 { Linear, Nearest };

struct TextureMip {
    u32 width = 0;
    u32 height = 0;
    std::vector<std::byte> data; // all layers/faces of this level, tightly packed, layer-major
};

struct TextureData {
    static constexpr AssetType kAssetType = AssetType::Texture;

    TextureFormat format = TextureFormat::RGBA8Srgb;
    u32 width = 0;  // of mip 0
    u32 height = 0;
    u32 layers = 1; // 6 for cubemaps (+X, -X, +Y, -Y, +Z, -Z — Vulkan order)
    bool cube = false;
    bool normalMap = false;
    TextureWrap wrapU = TextureWrap::Repeat;
    TextureWrap wrapV = TextureWrap::Repeat;
    TextureFilter filter = TextureFilter::Linear;
    u8 streamingPriority = 128; // higher = streamed earlier
    u32 mipCount = 0;           // levels in the file
    u32 firstMip = 0;           // first level present in `mips` (partial/streamed loads)
    std::vector<TextureMip> mips; // mips[i] is level firstMip + i

    [[nodiscard]] usize memoryUsage() const;
};

// Header of a .oxtex file without pixel data.
struct TextureFileInfo {
    TextureData desc; // mips empty, mipCount set
    struct Level {
        u64 offset = 0; // from file start
        u64 size = 0;
        u32 width = 0;
        u32 height = 0;
    };
    std::vector<Level> levels;
    u64 headerSize = 0; // bytes needed for readTextureInfo()
};

inline constexpr u64 kTextureMinHeaderRead = 4096; // enough for the header of any texture with ≤ 16 mips

[[nodiscard]] std::vector<std::byte> serializeTexture(const TextureData& texture);
// firstMip > 0 skips the largest levels (streaming start / low memory).
[[nodiscard]] Result<TextureData> deserializeTexture(std::span<const std::byte> data, u32 firstMip = 0);
[[nodiscard]] Result<TextureFileInfo> readTextureInfo(std::span<const std::byte> headerBytes);

// Byte-range reader: (offset, size) -> bytes. Pak entries / files / memory implement it.
using RangeReader = std::function<Result<std::vector<std::byte>>(u64 offset, u64 size)>;
// Levels [firstMip, lastMip] inclusive (clamped), read with one range request each.
[[nodiscard]] Result<std::vector<TextureMip>> readMipRange(const RangeReader& reader, const TextureFileInfo& info,
                                                           u32 firstMip, u32 lastMip);
[[nodiscard]] Result<std::vector<TextureMip>> readMipRange(const std::filesystem::path& file, u32 firstMip,
                                                           u32 lastMip);
[[nodiscard]] RangeReader fileRangeReader(std::filesystem::path file);

[[nodiscard]] TextureData makeCheckerTexture(u32 size = 64, u32 cell = 8);
[[nodiscard]] TextureData makeSolidTexture(u32 rgba, bool srgb, u32 size = 4);

} // namespace ox::assets
