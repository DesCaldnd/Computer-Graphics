#pragma once

#include <oxwald/assets/image.hpp>
#include <oxwald/assets/texture.hpp>

#include <array>
#include <filesystem>
#include <optional>

namespace ox::assets {

enum class TextureType : u8 { Color, Normal, Linear, HDR, UI };
// Default: BC7 (Color/Linear/UI), BC5 (Normal), RGBA16F (HDR — no BC6H encoder available).
enum class TextureCompression : u8 { Default, Uncompressed };
enum class CubemapMode : u8 { None, FromEquirect };

struct TextureImportSettings {
    TextureType type = TextureType::Color;
    // sRGB only applies to Color/UI. Linear data (masks, ORM) must use type Linear.
    bool srgb = true;
    u32 maxSize = 0; // 0 = unlimited; larger sources are downscaled (aspect kept, power of two not required)
    TextureCompression compression = TextureCompression::Default;
    bool generateMips = true;
    MipFilter mipFilter = MipFilter::Kaiser;
    bool preserveAlphaCoverage = false; // alpha-tested foliage etc.
    f32 alphaCutoff = 0.5f;
    bool flipY = false;
    TextureWrap wrapU = TextureWrap::Repeat;
    TextureWrap wrapV = TextureWrap::Repeat;
    TextureFilter filter = TextureFilter::Linear;
    u32 streamingPriority = 128;
    CubemapMode cubemap = CubemapMode::None;
    u32 cubeFaceSize = 0; // 0 = equirect height / 2
    u32 compressionQuality = 1; // UASTC level 0 (fastest) .. 4 (very slow) used for BC7
};

// Chooses the GPU format for settings (+ whether the image has alpha / HDR content).
[[nodiscard]] TextureFormat chooseTextureFormat(const TextureImportSettings& settings, bool hdrSource);

// Builds a TextureData from one image (2D) or six faces (cubemap), applying resize, mips and compression.
[[nodiscard]] Result<TextureData> buildTexture(std::span<const Image> faces, const TextureImportSettings& settings);
[[nodiscard]] Result<TextureData> importTexture(const Image& image, const TextureImportSettings& settings);
[[nodiscard]] Result<TextureData> importTextureFile(const std::filesystem::path& path,
                                                    const TextureImportSettings& settings);
// KTX2 (incl. Basis Universal supercompressed): Basis payloads are transcoded to BC7 (BC5 for type Normal);
// RGBA8/RGBA16F/BC5/BC7 payloads are taken over as-is.
[[nodiscard]] Result<TextureData> importKtx2(std::span<const std::byte> data, const TextureImportSettings& settings);

// ---- block compression -----------------------------------------------------------------------------------
// BC7 via Basis Universal UASTC (libktx encoder) transcoded to BC7. `image` values in [0,1] as stored.
[[nodiscard]] Result<std::vector<std::byte>> compressBC7(const Image& image, u32 quality = 1);
// Same, for several levels in one encoder invocation (much faster than per level).
[[nodiscard]] Result<std::vector<std::vector<std::byte>>> compressBC7Levels(std::span<const Image> levels,
                                                                            u32 quality = 1);
// BC5 from the R and G channels (own encoder, exhaustive endpoint refinement per 4x4 block).
[[nodiscard]] std::vector<std::byte> compressBC5(const Image& image);
[[nodiscard]] Image decodeBC7(std::span<const std::byte> blocks, u32 width, u32 height);
[[nodiscard]] Image decodeBC5(std::span<const std::byte> blocks, u32 width, u32 height); // B = reconstructed Z
// Raw encoders.
[[nodiscard]] std::vector<std::byte> encodeRGBA8(const Image& image);
[[nodiscard]] std::vector<std::byte> encodeRGBA16F(const Image& image);
[[nodiscard]] Image decodeTextureLevel(TextureFormat format, std::span<const std::byte> data, u32 width, u32 height);

// Fallback for devices without BC support: decodes BC levels into RGBA8 (sRGB preserved). No-op otherwise.
[[nodiscard]] TextureData decompressToRGBA8(const TextureData& texture);

// PSNR (dB) over the given channel mask (1=R,2=G,4=B,8=A) of two equally sized images, values in [0,1].
[[nodiscard]] f64 computePsnr(const Image& a, const Image& b, u32 channelMask = 7);

} // namespace ox::assets
