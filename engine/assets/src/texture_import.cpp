#include <oxwald/assets/texture_import.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/core/profile.hpp>

#include "internal.hpp"

#include <glm/gtc/packing.hpp>
#include <ktx.h>

#include <cmath>
#include <thread>

namespace ox::assets {

TextureFormat chooseTextureFormat(const TextureImportSettings& s, bool hdrSource) {
    const bool hdr = s.type == TextureType::HDR || hdrSource;
    const bool srgb = s.srgb && (s.type == TextureType::Color || s.type == TextureType::UI);
    if (hdr) return TextureFormat::RGBA16Float; // BC6H: no encoder in the dependency set
    if (s.compression == TextureCompression::Uncompressed) {
        return srgb ? TextureFormat::RGBA8Srgb : TextureFormat::RGBA8Unorm;
    }
    if (s.type == TextureType::Normal) return TextureFormat::BC5Unorm;
    return srgb ? TextureFormat::BC7Srgb : TextureFormat::BC7Unorm;
}

std::vector<std::byte> encodeRGBA8(const Image& image) {
    std::vector<std::byte> out(image.pixels.size() * 4);
    for (usize i = 0; i < image.pixels.size(); ++i) {
        for (int c = 0; c < 4; ++c) {
            out[i * 4 + c] = std::byte(u8(std::clamp(image.pixels[i][c], 0.0f, 1.0f) * 255.0f + 0.5f));
        }
    }
    return out;
}

std::vector<std::byte> encodeRGBA16F(const Image& image) {
    std::vector<std::byte> out(image.pixels.size() * 8);
    for (usize i = 0; i < image.pixels.size(); ++i) {
        for (int c = 0; c < 4; ++c) {
            const u16 h = u16(glm::packHalf1x16(std::min(image.pixels[i][c], 65504.0f)));
            std::memcpy(out.data() + i * 8 + c * 2, &h, 2);
        }
    }
    return out;
}

Result<std::vector<std::vector<std::byte>>> compressBC7Levels(std::span<const Image> levels, u32 quality) {
    OX_PROFILE_ZONE();
    if (levels.empty()) return std::vector<std::vector<std::byte>>{};
    ktxTextureCreateInfo ci{};
    ci.vkFormat = 37; // VK_FORMAT_R8G8B8A8_UNORM: encode the stored values as-is
    ci.baseWidth = levels[0].width;
    ci.baseHeight = levels[0].height;
    ci.baseDepth = 1;
    ci.numDimensions = 2;
    ci.numLevels = u32(levels.size());
    ci.numLayers = 1;
    ci.numFaces = 1;
    ci.isArray = KTX_FALSE;
    ci.generateMipmaps = KTX_FALSE;
    ktxTexture2* tex = nullptr;
    if (auto rc = ktxTexture2_Create(&ci, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &tex); rc != KTX_SUCCESS) {
        return makeError("ktxTexture2_Create: {}", ktxErrorString(rc));
    }
    struct Guard {
        ktxTexture2* t;
        ~Guard() { ktxTexture_Destroy(ktxTexture(t)); }
    } guard{tex};
    for (u32 l = 0; l < levels.size(); ++l) {
        const auto& img = levels[l];
        if (img.width != std::max(1u, ci.baseWidth >> l) || img.height != std::max(1u, ci.baseHeight >> l)) {
            return makeError("BC7: level {} has wrong size", l);
        }
        auto rgba = encodeRGBA8(img);
        if (auto rc = ktxTexture_SetImageFromMemory(ktxTexture(tex), l, 0, 0,
                                                    reinterpret_cast<const ktx_uint8_t*>(rgba.data()), rgba.size());
            rc != KTX_SUCCESS) {
            return makeError("ktxTexture_SetImageFromMemory: {}", ktxErrorString(rc));
        }
    }
    ktxBasisParams params{};
    params.structSize = sizeof(params);
    params.uastc = KTX_TRUE;
    params.threadCount = std::clamp<u32>(std::thread::hardware_concurrency(), 1, 16);
    params.uastcFlags = std::min<u32>(quality, KTX_PACK_UASTC_MAX_LEVEL);
    params.uastcRDO = KTX_FALSE;
    if (auto rc = ktxTexture2_CompressBasisEx(tex, &params); rc != KTX_SUCCESS) {
        return makeError("UASTC encode failed: {}", ktxErrorString(rc));
    }
    if (auto rc = ktxTexture2_TranscodeBasis(tex, KTX_TTF_BC7_RGBA, 0); rc != KTX_SUCCESS) {
        return makeError("UASTC -> BC7 transcode failed: {}", ktxErrorString(rc));
    }
    std::vector<std::vector<std::byte>> out;
    const auto* base = reinterpret_cast<const std::byte*>(ktxTexture_GetData(ktxTexture(tex)));
    for (u32 l = 0; l < levels.size(); ++l) {
        ktx_size_t offset = 0;
        ktxTexture_GetImageOffset(ktxTexture(tex), l, 0, 0, &offset);
        const usize size = ktxTexture_GetImageSize(ktxTexture(tex), l);
        out.emplace_back(base + offset, base + offset + size);
    }
    return out;
}

Result<std::vector<std::byte>> compressBC7(const Image& image, u32 quality) {
    auto levels = compressBC7Levels(std::span(&image, 1), quality);
    if (!levels) return levels.error();
    return std::move(levels->front());
}

Image decodeTextureLevel(TextureFormat format, std::span<const std::byte> data, u32 width, u32 height) {
    switch (format) {
    case TextureFormat::BC5Unorm: return decodeBC5(data, width, height);
    case TextureFormat::BC7Unorm:
    case TextureFormat::BC7Srgb: return decodeBC7(data, width, height);
    default: break;
    }
    Image img(width, height, glm::vec4(0, 0, 0, 1));
    const usize n = usize(width) * height;
    if (data.size() < n * formatBlockBytes(format)) return img;
    const auto* b = reinterpret_cast<const u8*>(data.data());
    for (usize i = 0; i < n; ++i) {
        glm::vec4& p = img.pixels[i];
        switch (format) {
        case TextureFormat::R8Unorm: p = glm::vec4(b[i] / 255.0f, 0, 0, 1); break;
        case TextureFormat::RG8Unorm: p = glm::vec4(b[i * 2] / 255.0f, b[i * 2 + 1] / 255.0f, 0, 1); break;
        case TextureFormat::RGBA8Unorm:
        case TextureFormat::RGBA8Srgb:
            p = glm::vec4(b[i * 4], b[i * 4 + 1], b[i * 4 + 2], b[i * 4 + 3]) / 255.0f;
            break;
        case TextureFormat::RGBA16Float:
            for (int c = 0; c < 4; ++c) {
                u16 h;
                std::memcpy(&h, b + i * 8 + c * 2, 2);
                p[c] = glm::unpackHalf1x16(h);
            }
            img.hdr = true;
            break;
        case TextureFormat::RGBA32Float:
            std::memcpy(&p, b + i * 16, 16);
            img.hdr = true;
            break;
        case TextureFormat::R16Unorm: {
            u16 v;
            std::memcpy(&v, b + i * 2, 2);
            p = glm::vec4(v / 65535.0f, 0, 0, 1);
            break;
        }
        case TextureFormat::R32Float: {
            f32 v;
            std::memcpy(&v, b + i * 4, 4);
            p = glm::vec4(v, 0, 0, 1);
            break;
        }
        default: break;
        }
    }
    return img;
}

TextureData decompressToRGBA8(const TextureData& tex) {
    if (!isBlockCompressed(tex.format)) return tex;
    TextureData out = tex;
    out.format = tex.format == TextureFormat::BC7Srgb ? TextureFormat::RGBA8Srgb : TextureFormat::RGBA8Unorm;
    for (auto& mip : out.mips) {
        const u64 faceSize = textureLevelSize(tex.format, mip.width, mip.height);
        std::vector<std::byte> data;
        for (u32 layer = 0; layer < tex.layers; ++layer) {
            const auto face = std::span(mip.data).subspan(layer * faceSize, faceSize);
            auto rgba = encodeRGBA8(decodeTextureLevel(tex.format, face, mip.width, mip.height));
            data.insert(data.end(), rgba.begin(), rgba.end());
        }
        mip.data = std::move(data);
    }
    return out;
}

f64 computePsnr(const Image& a, const Image& b, u32 mask) {
    if (a.width != b.width || a.height != b.height || a.pixels.empty()) return 0.0;
    f64 sum = 0.0;
    u64 count = 0;
    for (usize i = 0; i < a.pixels.size(); ++i) {
        for (int c = 0; c < 4; ++c) {
            if (!(mask & (1u << c))) continue;
            const f64 d = f64(std::clamp(a.pixels[i][c], 0.0f, 1.0f)) - f64(std::clamp(b.pixels[i][c], 0.0f, 1.0f));
            sum += d * d;
            ++count;
        }
    }
    if (count == 0) return 0.0;
    const f64 mse = sum / f64(count);
    if (mse <= 1e-12) return 99.0;
    return 10.0 * std::log10(1.0 / mse);
}

Result<TextureData> buildTexture(std::span<const Image> inputFaces, const TextureImportSettings& s) {
    OX_PROFILE_ZONE();
    if (inputFaces.empty() || (inputFaces.size() != 1 && inputFaces.size() != 6)) {
        return makeError("buildTexture needs 1 image or 6 cube faces");
    }
    std::vector<Image> faces(inputFaces.begin(), inputFaces.end());
    for (const auto& f : faces) {
        if (f.empty()) return makeError("empty image");
        if (f.width != faces[0].width || f.height != faces[0].height) return makeError("cube faces differ in size");
    }
    bool hdrSource = false;
    for (const auto& f : faces) hdrSource |= f.hdr;
    const TextureFormat format = chooseTextureFormat(s, hdrSource);
    const bool srgb = s.srgb && (s.type == TextureType::Color || s.type == TextureType::UI) &&
                      format != TextureFormat::RGBA16Float;

    for (auto& f : faces) {
        if (s.flipY) {
            for (u32 y = 0; y < f.height / 2; ++y) {
                for (u32 x = 0; x < f.width; ++x) std::swap(f.at(x, y), f.at(x, f.height - 1 - y));
            }
        }
        if (s.maxSize > 0 && std::max(f.width, f.height) > s.maxSize) {
            const f32 k = f32(s.maxSize) / f32(std::max(f.width, f.height));
            f = resizeImage(f, std::max(1u, u32(std::lround(f.width * k))), std::max(1u, u32(std::lround(f.height * k))),
                            s.mipFilter, srgb);
        }
        if (s.type == TextureType::Normal) {
            // Sources may be unnormalised (8-bit quantisation, painted maps): normalise level 0 too.
            for (auto& p : f.pixels) {
                glm::vec3 n = glm::vec3(p) * 2.0f - 1.0f;
                const f32 len = glm::length(n);
                n = len > 1e-6f ? n / len : glm::vec3(0, 0, 1);
                p = glm::vec4(n * 0.5f + 0.5f, 1.0f);
            }
        }
    }

    MipSettings ms;
    ms.filter = s.mipFilter;
    ms.srgb = srgb;
    ms.normalMap = s.type == TextureType::Normal;
    ms.preserveAlphaCoverage = s.preserveAlphaCoverage;
    ms.alphaCutoff = s.alphaCutoff;

    std::vector<std::vector<Image>> chains;
    for (const auto& f : faces) chains.push_back(s.generateMips ? generateMipChain(f, ms) : std::vector<Image>{f});

    TextureData tex;
    tex.format = format;
    tex.width = faces[0].width;
    tex.height = faces[0].height;
    tex.layers = u32(faces.size());
    tex.cube = faces.size() == 6;
    tex.normalMap = s.type == TextureType::Normal;
    tex.wrapU = s.wrapU;
    tex.wrapV = s.wrapV;
    tex.filter = s.filter;
    tex.streamingPriority = u8(std::min<u32>(s.streamingPriority, 255));
    const u32 levelCount = u32(chains[0].size());
    tex.mips.resize(levelCount);
    for (u32 l = 0; l < levelCount; ++l) {
        tex.mips[l].width = chains[0][l].width;
        tex.mips[l].height = chains[0][l].height;
    }
    for (const auto& chain : chains) {
        std::vector<std::vector<std::byte>> encoded;
        switch (format) {
        case TextureFormat::BC7Unorm:
        case TextureFormat::BC7Srgb: {
            auto bc = compressBC7Levels(chain, s.compressionQuality);
            if (!bc) return bc.error();
            encoded = std::move(*bc);
            break;
        }
        case TextureFormat::BC5Unorm:
            for (const auto& img : chain) encoded.push_back(compressBC5(img));
            break;
        case TextureFormat::RGBA16Float:
            for (const auto& img : chain) encoded.push_back(encodeRGBA16F(img));
            break;
        default:
            for (const auto& img : chain) encoded.push_back(encodeRGBA8(img));
            break;
        }
        for (u32 l = 0; l < levelCount; ++l) {
            if (encoded[l].size() != textureLevelSize(format, tex.mips[l].width, tex.mips[l].height)) {
                return makeError("encoder produced {} bytes for level {}, expected {}", encoded[l].size(), l,
                                 textureLevelSize(format, tex.mips[l].width, tex.mips[l].height));
            }
            tex.mips[l].data.insert(tex.mips[l].data.end(), encoded[l].begin(), encoded[l].end());
        }
    }
    tex.mipCount = levelCount;
    return tex;
}

Result<TextureData> importTexture(const Image& image, const TextureImportSettings& settings) {
    if (settings.cubemap == CubemapMode::FromEquirect) {
        const u32 faceSize = settings.cubeFaceSize ? settings.cubeFaceSize : std::max(1u, image.height / 2);
        auto faces = equirectToCubemap(image, faceSize);
        return buildTexture(faces, settings);
    }
    return buildTexture(std::span(&image, 1), settings);
}

Result<TextureData> importTextureFile(const std::filesystem::path& path, const TextureImportSettings& settings) {
    if (detail::toLower(path.extension().string()) == ".ktx2") {
        auto bytes = detail::readFile(path);
        if (!bytes) return bytes.error();
        return importKtx2(*bytes, settings);
    }
    auto img = loadImage(path);
    if (!img) return img.error();
    return importTexture(*img, settings);
}

namespace {
TextureFormat fromVkFormat(u32 vk) {
    switch (vk) {
    case 9: return TextureFormat::R8Unorm;
    case 16: return TextureFormat::RG8Unorm;
    case 37: return TextureFormat::RGBA8Unorm;
    case 43: return TextureFormat::RGBA8Srgb;
    case 97: return TextureFormat::RGBA16Float;
    case 109: return TextureFormat::RGBA32Float;
    case 141: return TextureFormat::BC5Unorm;
    case 143: return TextureFormat::BC6HUfloat;
    case 145: return TextureFormat::BC7Unorm;
    case 146: return TextureFormat::BC7Srgb;
    case 70: return TextureFormat::R16Unorm;
    case 100: return TextureFormat::R32Float;
    default: return TextureFormat::Unknown;
    }
}
} // namespace

Result<TextureData> importKtx2(std::span<const std::byte> data, const TextureImportSettings& s) {
    OX_PROFILE_ZONE();
    ktxTexture2* tex = nullptr;
    if (auto rc = ktxTexture2_CreateFromMemory(reinterpret_cast<const ktx_uint8_t*>(data.data()), data.size(),
                                               KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &tex);
        rc != KTX_SUCCESS) {
        return makeError("KTX2 parse failed: {}", ktxErrorString(rc));
    }
    struct Guard {
        ktxTexture2* t;
        ~Guard() { ktxTexture_Destroy(ktxTexture(t)); }
    } guard{tex};
    if (ktxTexture2_NeedsTranscoding(tex)) {
        const auto target = s.type == TextureType::Normal ? KTX_TTF_BC5_RG : KTX_TTF_BC7_RGBA;
        if (auto rc = ktxTexture2_TranscodeBasis(tex, target, 0); rc != KTX_SUCCESS) {
            return makeError("Basis transcode failed: {}", ktxErrorString(rc));
        }
    }
    TextureFormat format = fromVkFormat(tex->vkFormat);
    if (format == TextureFormat::Unknown) return makeError("unsupported KTX2 vkFormat {}", tex->vkFormat);
    // Basis payloads are colour-space agnostic; apply the import settings.
    const bool wantSrgb = s.srgb && (s.type == TextureType::Color || s.type == TextureType::UI);
    if (format == TextureFormat::BC7Unorm && wantSrgb) format = TextureFormat::BC7Srgb;
    if (tex->numDimensions != 2 || tex->baseDepth > 1) return makeError("only 2D/cube KTX2 textures are supported");
    TextureData out;
    out.format = format;
    out.width = tex->baseWidth;
    out.height = tex->baseHeight;
    out.cube = tex->isCubemap;
    out.layers = tex->numFaces * std::max(1u, tex->numLayers);
    out.normalMap = s.type == TextureType::Normal;
    out.wrapU = s.wrapU;
    out.wrapV = s.wrapV;
    out.filter = s.filter;
    out.streamingPriority = u8(std::min<u32>(s.streamingPriority, 255));
    const auto* base = reinterpret_cast<const std::byte*>(ktxTexture_GetData(ktxTexture(tex)));
    for (u32 l = 0; l < tex->numLevels; ++l) {
        TextureMip mip;
        mip.width = std::max(1u, tex->baseWidth >> l);
        mip.height = std::max(1u, tex->baseHeight >> l);
        const usize faceSize = ktxTexture_GetImageSize(ktxTexture(tex), l);
        for (u32 layer = 0; layer < std::max(1u, tex->numLayers); ++layer) {
            for (u32 face = 0; face < tex->numFaces; ++face) {
                ktx_size_t offset = 0;
                ktxTexture_GetImageOffset(ktxTexture(tex), l, layer, face, &offset);
                mip.data.insert(mip.data.end(), base + offset, base + offset + faceSize);
            }
        }
        out.mips.push_back(std::move(mip));
    }
    out.mipCount = u32(out.mips.size());
    return out;
}

} // namespace ox::assets
