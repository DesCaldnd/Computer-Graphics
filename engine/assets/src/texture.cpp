#include <oxwald/assets/texture.hpp>

#include "internal.hpp"

#include <fstream>

namespace ox::assets {

namespace {
constexpr u32 kTextureVersion = 1;
constexpr u16 kFlagCube = 1;
constexpr u16 kFlagNormal = 2;

#pragma pack(push, 1)
struct TexHeader {
    char magic[4];
    u32 version;
    u16 format;
    u16 flags;
    u32 width;
    u32 height;
    u32 layers;
    u32 mipCount;
    u8 wrapU;
    u8 wrapV;
    u8 filter;
    u8 streamingPriority;
    u32 headerSize; // header + mip table
    u32 tableCrc;
    u8 reserved[24];
};
struct TexLevel {
    u64 offset;
    u64 size;
    u32 width;
    u32 height;
    u32 crc;
    u32 reserved;
};
#pragma pack(pop)
static_assert(sizeof(TexHeader) == 64);
static_assert(sizeof(TexLevel) == 32);
} // namespace

std::string_view textureFormatName(TextureFormat f) {
    switch (f) {
    case TextureFormat::R8Unorm: return "R8_UNORM";
    case TextureFormat::RG8Unorm: return "RG8_UNORM";
    case TextureFormat::RGBA8Unorm: return "RGBA8_UNORM";
    case TextureFormat::RGBA8Srgb: return "RGBA8_SRGB";
    case TextureFormat::RGBA16Float: return "RGBA16_SFLOAT";
    case TextureFormat::RGBA32Float: return "RGBA32_SFLOAT";
    case TextureFormat::BC5Unorm: return "BC5_UNORM";
    case TextureFormat::BC7Unorm: return "BC7_UNORM";
    case TextureFormat::BC7Srgb: return "BC7_SRGB";
    case TextureFormat::BC6HUfloat: return "BC6H_UFLOAT";
    case TextureFormat::R16Unorm: return "R16_UNORM";
    case TextureFormat::R32Float: return "R32_SFLOAT";
    default: return "UNKNOWN";
    }
}

u32 toVkFormat(TextureFormat f) {
    switch (f) {
    case TextureFormat::R8Unorm: return 9;       // VK_FORMAT_R8_UNORM
    case TextureFormat::RG8Unorm: return 16;     // VK_FORMAT_R8G8_UNORM
    case TextureFormat::RGBA8Unorm: return 37;   // VK_FORMAT_R8G8B8A8_UNORM
    case TextureFormat::RGBA8Srgb: return 43;    // VK_FORMAT_R8G8B8A8_SRGB
    case TextureFormat::RGBA16Float: return 97;  // VK_FORMAT_R16G16B16A16_SFLOAT
    case TextureFormat::RGBA32Float: return 109; // VK_FORMAT_R32G32B32A32_SFLOAT
    case TextureFormat::BC5Unorm: return 141;    // VK_FORMAT_BC5_UNORM_BLOCK
    case TextureFormat::BC6HUfloat: return 143;  // VK_FORMAT_BC6H_UFLOAT_BLOCK
    case TextureFormat::BC7Unorm: return 145;    // VK_FORMAT_BC7_UNORM_BLOCK
    case TextureFormat::BC7Srgb: return 146;     // VK_FORMAT_BC7_SRGB_BLOCK
    case TextureFormat::R16Unorm: return 70;     // VK_FORMAT_R16_UNORM
    case TextureFormat::R32Float: return 100;    // VK_FORMAT_R32_SFLOAT
    default: return 0;
    }
}

bool isBlockCompressed(TextureFormat f) {
    return f == TextureFormat::BC5Unorm || f == TextureFormat::BC7Unorm || f == TextureFormat::BC7Srgb ||
           f == TextureFormat::BC6HUfloat;
}

bool isSrgb(TextureFormat f) { return f == TextureFormat::RGBA8Srgb || f == TextureFormat::BC7Srgb; }

u32 formatBlockBytes(TextureFormat f) {
    switch (f) {
    case TextureFormat::R8Unorm: return 1;
    case TextureFormat::RG8Unorm: return 2;
    case TextureFormat::R16Unorm: return 2;
    case TextureFormat::RGBA8Unorm:
    case TextureFormat::RGBA8Srgb:
    case TextureFormat::R32Float: return 4;
    case TextureFormat::RGBA16Float: return 8;
    case TextureFormat::RGBA32Float: return 16;
    case TextureFormat::BC5Unorm:
    case TextureFormat::BC6HUfloat:
    case TextureFormat::BC7Unorm:
    case TextureFormat::BC7Srgb: return 16;
    default: return 0;
    }
}

u64 textureLevelSize(TextureFormat f, u32 width, u32 height) {
    if (isBlockCompressed(f)) return u64((width + 3) / 4) * ((height + 3) / 4) * formatBlockBytes(f);
    return u64(width) * height * formatBlockBytes(f);
}

usize TextureData::memoryUsage() const {
    usize n = sizeof(TextureData);
    for (const auto& m : mips) n += m.data.size();
    return n;
}

std::vector<std::byte> serializeTexture(const TextureData& tex) {
    ByteWriter w;
    TexHeader h{};
    std::memcpy(h.magic, "OXTX", 4);
    h.version = kTextureVersion;
    h.format = u16(tex.format);
    h.flags = u16((tex.cube ? kFlagCube : 0) | (tex.normalMap ? kFlagNormal : 0));
    h.width = tex.width;
    h.height = tex.height;
    h.layers = tex.layers;
    h.mipCount = static_cast<u32>(tex.mips.size());
    h.wrapU = u8(tex.wrapU);
    h.wrapV = u8(tex.wrapV);
    h.filter = u8(tex.filter);
    h.streamingPriority = tex.streamingPriority;
    h.headerSize = u32(sizeof(TexHeader) + sizeof(TexLevel) * tex.mips.size());
    w.write(h);
    std::vector<TexLevel> table(tex.mips.size());
    w.writeBytes(table.data(), table.size() * sizeof(TexLevel));
    // Smallest level first: header + the whole mip tail is one contiguous prefix of the file.
    for (usize i = tex.mips.size(); i-- > 0;) {
        w.align(16);
        const auto& m = tex.mips[i];
        table[i] = TexLevel{w.size(), m.data.size(), m.width, m.height, crc32(m.data), 0};
        w.writeBytes(m.data.data(), m.data.size());
    }
    for (usize i = 0; i < table.size(); ++i) w.patch(sizeof(TexHeader) + i * sizeof(TexLevel), table[i]);
    const u32 tableCrc = crc32(w.data().data() + sizeof(TexHeader), table.size() * sizeof(TexLevel));
    w.patch(offsetof(TexHeader, tableCrc), tableCrc);
    return w.take();
}

Result<TextureFileInfo> readTextureInfo(std::span<const std::byte> bytes) {
    ByteReader r(bytes);
    const TexHeader h = r.read<TexHeader>();
    if (r.failed() || std::memcmp(h.magic, "OXTX", 4) != 0) return makeError("not an .oxtex file");
    if (h.version > kTextureVersion) return makeError("texture version {} is newer than supported", h.version);
    if (h.mipCount > 32 || h.layers == 0 || h.layers > 2048) return makeError("corrupt texture header");
    TextureFileInfo info;
    info.headerSize = h.headerSize;
    info.desc.format = TextureFormat(h.format);
    info.desc.cube = (h.flags & kFlagCube) != 0;
    info.desc.normalMap = (h.flags & kFlagNormal) != 0;
    info.desc.width = h.width;
    info.desc.height = h.height;
    info.desc.layers = h.layers;
    info.desc.mipCount = h.mipCount;
    info.desc.wrapU = TextureWrap(h.wrapU);
    info.desc.wrapV = TextureWrap(h.wrapV);
    info.desc.filter = TextureFilter(h.filter);
    info.desc.streamingPriority = h.streamingPriority;
    if (bytes.size() < sizeof(TexHeader) + h.mipCount * sizeof(TexLevel)) {
        return makeError("texture header truncated (need {} bytes)", h.headerSize);
    }
    if (crc32(bytes.data() + sizeof(TexHeader), h.mipCount * sizeof(TexLevel)) != h.tableCrc) {
        return makeError("texture mip table CRC mismatch");
    }
    for (u32 i = 0; i < h.mipCount; ++i) {
        const TexLevel l = r.read<TexLevel>();
        info.levels.push_back({l.offset, l.size, l.width, l.height});
        if (l.size != textureLevelSize(info.desc.format, l.width, l.height) * h.layers) {
            return makeError("texture level {} has an unexpected size", i);
        }
    }
    return info;
}

Result<TextureData> deserializeTexture(std::span<const std::byte> data, u32 firstMip) {
    auto info = readTextureInfo(data);
    if (!info) return info.error();
    TextureData tex = info->desc;
    tex.firstMip = std::min(firstMip, tex.mipCount ? tex.mipCount - 1 : 0);
    ByteReader r(data);
    r.seek(sizeof(TexHeader));
    for (u32 i = 0; i < tex.mipCount; ++i) {
        const TexLevel l = r.read<TexLevel>();
        if (i < tex.firstMip) continue;
        if (l.offset > data.size() || l.size > data.size() - l.offset) return makeError("texture level {} out of range", i);
        TextureMip m;
        m.width = l.width;
        m.height = l.height;
        m.data.assign(data.begin() + l.offset, data.begin() + l.offset + l.size);
        if (crc32(m.data) != l.crc) return makeError("texture level {} CRC mismatch", i);
        tex.mips.push_back(std::move(m));
    }
    return tex;
}

Result<std::vector<TextureMip>> readMipRange(const RangeReader& reader, const TextureFileInfo& info, u32 firstMip,
                                             u32 lastMip) {
    std::vector<TextureMip> out;
    if (info.levels.empty()) return out;
    lastMip = std::min<u32>(lastMip, u32(info.levels.size()) - 1);
    for (u32 i = firstMip; i <= lastMip; ++i) {
        const auto& l = info.levels[i];
        auto bytes = reader(l.offset, l.size);
        if (!bytes) return bytes.error();
        if (bytes->size() != l.size) return makeError("short read for texture level {}", i);
        out.push_back(TextureMip{l.width, l.height, std::move(*bytes)});
    }
    return out;
}

RangeReader fileRangeReader(std::filesystem::path file) {
    return [file = std::move(file)](u64 offset, u64 size) -> Result<std::vector<std::byte>> {
        std::ifstream in(file, std::ios::binary);
        if (!in) return makeError("cannot open {}", file.string());
        in.seekg(std::streamoff(offset));
        std::vector<std::byte> out(size);
        in.read(reinterpret_cast<char*>(out.data()), std::streamsize(size));
        out.resize(usize(in.gcount()));
        return out;
    };
}

Result<std::vector<TextureMip>> readMipRange(const std::filesystem::path& file, u32 firstMip, u32 lastMip) {
    auto reader = fileRangeReader(file);
    auto header = reader(0, kTextureMinHeaderRead);
    if (!header) return header.error();
    auto info = readTextureInfo(*header);
    if (!info && header->size() >= sizeof(TexHeader)) {
        // Large mip tables: read exactly the announced header size.
        const auto* h = reinterpret_cast<const TexHeader*>(header->data());
        header = reader(0, h->headerSize);
        if (!header) return header.error();
        info = readTextureInfo(*header);
    }
    if (!info) return info.error();
    return readMipRange(reader, *info, firstMip, lastMip);
}

TextureData makeSolidTexture(u32 rgba, bool srgb, u32 size) {
    TextureData tex;
    tex.format = srgb ? TextureFormat::RGBA8Srgb : TextureFormat::RGBA8Unorm;
    tex.width = tex.height = size;
    for (u32 s = size;; s = std::max(1u, s / 2)) {
        TextureMip m{s, s, std::vector<std::byte>(usize(s) * s * 4)};
        for (usize i = 0; i < usize(s) * s; ++i) std::memcpy(m.data.data() + i * 4, &rgba, 4);
        tex.mips.push_back(std::move(m));
        if (s == 1) break;
    }
    tex.mipCount = u32(tex.mips.size());
    return tex;
}

TextureData makeCheckerTexture(u32 size, u32 cell) {
    TextureData tex;
    tex.format = TextureFormat::RGBA8Srgb;
    tex.width = tex.height = size;
    tex.filter = TextureFilter::Nearest;
    const u32 a = 0xffff00ffu; // magenta
    const u32 b = 0xff404040u; // dark grey
    for (u32 s = size, c = cell;; s = std::max(1u, s / 2), c = std::max(1u, c / 2)) {
        TextureMip m{s, s, std::vector<std::byte>(usize(s) * s * 4)};
        for (u32 y = 0; y < s; ++y) {
            for (u32 x = 0; x < s; ++x) {
                const u32 v = (((x / c) + (y / c)) & 1) ? a : b;
                std::memcpy(m.data.data() + (usize(y) * s + x) * 4, &v, 4);
            }
        }
        tex.mips.push_back(std::move(m));
        if (s == 1) break;
    }
    tex.mipCount = u32(tex.mips.size());
    return tex;
}

} // namespace ox::assets
