#pragma once

// CPU images used by the texture pipeline: float RGBA texels in their stored encoding (sRGB-encoded colour
// stays sRGB-encoded; the mip/resize functions linearise when told the data is sRGB).

#include <oxwald/core/math.hpp>
#include <oxwald/core/result.hpp>

#include <array>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace ox::assets {

struct Image {
    u32 width = 0;
    u32 height = 0;
    u32 channels = 4; // channels present in the source (informational; pixels are always RGBA)
    bool hdr = false; // values may exceed 1 (HDR/EXR sources)
    std::vector<glm::vec4> pixels;

    Image() = default;
    Image(u32 w, u32 h, glm::vec4 fill = glm::vec4(0.0f)) : width(w), height(h), pixels(usize(w) * h, fill) {}

    [[nodiscard]] bool empty() const { return pixels.empty(); }
    [[nodiscard]] glm::vec4& at(u32 x, u32 y) { return pixels[usize(y) * width + x]; }
    [[nodiscard]] const glm::vec4& at(u32 x, u32 y) const { return pixels[usize(y) * width + x]; }
    // Bilinear sample with wrap (u) / clamp (v) addressing, uv in [0,1].
    [[nodiscard]] glm::vec4 sampleBilinear(glm::vec2 uv, bool wrapU = true, bool wrapV = false) const;
};

// PNG/JPG/TGA/BMP/PSD/GIF/HDR (stb_image) and EXR (tinyexr). 16-bit PNGs keep their precision.
[[nodiscard]] Result<Image> loadImage(const std::filesystem::path& path);
// extensionHint: "png", "exr", ... (only needed for EXR; others are detected from content).
[[nodiscard]] Result<Image> loadImageFromMemory(std::span<const std::byte> data, std::string_view extensionHint = {});
// PNG writer (8-bit, values clamped) — debugging and tests.
Status saveImagePng(const std::filesystem::path& path, const Image& image);
[[nodiscard]] std::vector<std::byte> encodePng(const Image& image);

[[nodiscard]] f32 srgbToLinear(f32 v);
[[nodiscard]] f32 linearToSrgb(f32 v);

enum class MipFilter : u8 { Box, Kaiser };

struct MipSettings {
    MipFilter filter = MipFilter::Kaiser;
    bool srgb = false;      // RGB is sRGB-encoded: filter in linear space
    bool normalMap = false; // RGB = n*0.5+0.5: renormalise every level
    bool preserveAlphaCoverage = false;
    f32 alphaCutoff = 0.5f;
};

[[nodiscard]] Image resizeImage(const Image& src, u32 width, u32 height, MipFilter filter = MipFilter::Kaiser,
                                bool srgb = false);
// Full chain down to 1x1 (or maxLevels levels), level 0 = copy of `base`.
[[nodiscard]] std::vector<Image> generateMipChain(const Image& base, const MipSettings& settings, u32 maxLevels = 0);
[[nodiscard]] u32 mipLevelCount(u32 width, u32 height);
// Fraction of texels with alpha * scale >= cutoff.
[[nodiscard]] f32 alphaCoverage(const Image& image, f32 cutoff, f32 scale = 1.0f);

// Faces in Vulkan order +X, -X, +Y, -Y, +Z, -Z. Equirect: u = atan2(d.x, -d.z)/2π + 0.5, v = acos(d.y)/π.
[[nodiscard]] std::array<Image, 6> equirectToCubemap(const Image& equirect, u32 faceSize);
// Direction of the texel centre (u,v ∈ [0,1]) of a cube face (Vulkan cube addressing).
[[nodiscard]] glm::vec3 cubeFaceDirection(u32 face, f32 u, f32 v);

} // namespace ox::assets
