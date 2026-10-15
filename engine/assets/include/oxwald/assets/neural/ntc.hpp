#pragma once

// Neural texture compression (NTC), in the spirit of Vaidyanathan et al., "Random-Access Neural Compression of
// Material Textures" (SIGGRAPH 2023). All textures of a material (albedo + alpha, normal XY, occlusion / roughness /
// metallic, emissive) are encoded together into
//   * a pyramid of quantised latent feature grids: one pair (G0, G1) per two texture mips, G0 at 1/stride of the
//     pair's first mip, G1 at half of G0 (4- or 8-bit features, four per texel of a 2D array layer),
//   * a tiny MLP decoder (inputs: bilinear G0 + G1 features, positional encoding of the texel inside the G0 cell,
//     mip parity; 2 hidden ReLU layers of 32 or 64; one output per channel), FP16 weights,
//   * a BC-compressed fallback mip tail (mips at most `fallbackSize`), used for distant / low-detail sampling, by
//     consumers without inference (ray tracing hit shading, shadow alpha test, captures) and as the regular mips of
//     decoded textures.
// The decoder is evaluated at texel centres only; the renderer's InferenceOnSample mode picks one texel (and mip)
// stochastically per sample (stochastic texture filtering) and lets TAA integrate the filter.
//
//   NtcSourceImages src{.albedo = loadImage(...), .normal = ..., .orm = ...};
//   auto set = encodeNeuralTextures(src, NeuralTextureSettings{.preset = NtcPreset::Medium});
//   std::vector<std::byte> bytes = serializeNeuralTextureSet(*set);            // .oxntc
//   NtcDecodedTextures tex = decodeToTextures(*set);                            // BC7 / BC5 (DecodeOnLoad)

#include <oxwald/assets/asset_types.hpp>
#include <oxwald/assets/image.hpp>
#include <oxwald/assets/neural/mlp.hpp>
#include <oxwald/assets/texture.hpp>
#include <oxwald/core/result.hpp>

#include <array>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace ox::assets {

enum class NtcPreset : u8 { Low, Medium, High, Custom };
enum class NtcLatentFormat : u8 { Q4, Q8 };

// Material import option "neural" (see MaterialImportSettings) and the parameters of encodeNeuralTextures().
struct NeuralTextureSettings {
    NtcPreset preset = NtcPreset::Medium;
    // Picks the preset whose estimated bits per texel (all channels together, mip 0) is closest to this value
    // when > 0 (BC7 albedo + BC5 normal + BC7 ORM = 24).
    f32 targetBitsPerTexel = 0.0f;
    // Custom preset only.
    u32 latentStride = 4;  // G0 texel covers stride × stride texels of the pair's first mip
    u32 features0 = 8;     // G0 features (multiple of 4, <= 16)
    u32 features1 = 8;     // G1 features (multiple of 4, <= 8)
    NtcLatentFormat latentFormat = NtcLatentFormat::Q4;
    u32 hidden = 64;       // hidden layer width: 32 or 64
    // Training.
    u32 steps = 0;         // 0 = automatic (from texel count and preset)
    u32 batch = 4096;
    f32 learningRate = 0.01f;       // network (latents use 10×)
    u64 seed = 1;
    u32 threads = 0;                // 0 = hardware concurrency; never changes the result
    // Mips with max(width, height) <= fallbackSize are stored as BC textures instead of latents.
    u32 fallbackSize = 128;
    u32 maxSize = 0;                // downscale the sources first (0 = keep)
};

// Settings of the "material" importer (.oxmat metas). Off: plain material. On: also trains a NeuralTextureSet
// sub-asset ("<material>#NeuralTextures") and keeps the regular textures as dependencies. Replace: the material
// depends only on the neural set (cooked games do not ship the regular textures).
enum class NeuralCompressionMode : u8 { Off, On, Replace };
struct MaterialImportSettings {
    NeuralCompressionMode neuralCompression = NeuralCompressionMode::Off;
    NtcPreset neuralPreset = NtcPreset::Medium;
    f32 neuralTargetBitsPerTexel = 0.0f;
    u32 neuralLatentStride = 4;
    u32 neuralFeatures0 = 8;
    u32 neuralFeatures1 = 8;
    NtcLatentFormat neuralLatentFormat = NtcLatentFormat::Q4;
    u32 neuralHidden = 64;
    u32 neuralSteps = 0;
    u32 neuralFallbackSize = 128;
    u32 neuralMaxSize = 0;
    u32 neuralSeed = 1;

    [[nodiscard]] NeuralTextureSettings toNeuralSettings() const;
};

// Network input layout (fixed positions, so shaders index constants): G0 features at [0, 16), G1 features at
// [16, 24), sin/cos of the position inside the G0 cell (x, y) at [24, 28), mip parity at 28, zeros up to 32.
inline constexpr u32 kNtcInputs = 32;  // padded network input width
inline constexpr u32 kNtcOutputs = 16; // padded network output width
inline constexpr u32 kNtcMaxFeatures0 = 16;
inline constexpr u32 kNtcMaxFeatures1 = 8;
inline constexpr u32 kNtcG1Offset = 16;
inline constexpr u32 kNtcPositionalOffset = 24;
inline constexpr u32 kNtcMaxPairs = 4; // neural mips <= 8; larger sets keep more mips in the fallback

// Channel offsets of the decoder outputs (-1 = absent). Albedo / emissive are sRGB encoded, normal is n.xy*0.5+0.5,
// ORM is linear (occlusion, roughness, metallic).
struct NtcChannelLayout {
    i32 albedo = -1;   // 3 channels
    i32 alpha = -1;    // 1
    i32 normal = -1;   // 2
    i32 orm = -1;      // 3
    i32 emissive = -1; // 3
    u32 count = 0;
};

struct NtcLatentGrid {
    u32 width = 0, height = 0;
    u32 features = 0;           // multiple of 4: layers = features / 4
    // Quantised values (0 .. 2^bits - 1), layout [layer][y][x][4].
    std::vector<u8> values;
    [[nodiscard]] u32 layers() const { return (features + 3) / 4; }
};

struct NtcLevelPair {
    std::array<NtcLatentGrid, 2> grids;
};

struct NtcChannelStats {
    std::string name;
    f64 psnr = 0.0;     // decoded mip 0 vs. the source, dB
    f64 ssim = 0.0;
    f64 bcPsnr = 0.0;   // BC7 / BC5 of the same source (baseline), dB
    f64 bcSsim = 0.0;
};

struct NtcStats {
    std::vector<NtcChannelStats> channels; // "albedo", "alpha", "normal", "occlusion", "roughness", "metallic", "emissive"
    f64 trainMs = 0.0;
    u32 steps = 0;
    f32 finalLoss = 0.0f;
    u64 sourceBcBytes = 0; // BC7/BC5 textures with full mip chains for the same sources (baseline)
};

struct NeuralTextureSet {
    static constexpr AssetType kAssetType = AssetType::NeuralTextureSet;

    u32 width = 0, height = 0;   // mip 0
    u32 mipCount = 0;            // full chain
    u32 neuralMips = 0;          // mips [0, neuralMips) are decoded by the network
    u32 latentStride = 4;
    NtcLatentFormat latentFormat = NtcLatentFormat::Q4;
    NtcPreset preset = NtcPreset::Medium;
    NtcChannelLayout channels;
    nn::Mlp network;             // FP16-representable parameters, kNtcInputs -> hidden -> hidden -> kNtcOutputs
    std::vector<NtcLevelPair> pairs;
    // Mips [neuralMips, mipCount): albedo BC7 sRGB, normal BC5, ORM BC7, emissive BC7 sRGB (format Unknown = absent).
    std::array<TextureData, 4> fallback;
    NtcStats stats;

    [[nodiscard]] u32 hidden() const;
    [[nodiscard]] u32 latentBits() const { return latentFormat == NtcLatentFormat::Q4 ? 4u : 8u; }
    [[nodiscard]] u32 mipWidth(u32 mip) const { return std::max(1u, width >> mip); }
    [[nodiscard]] u32 mipHeight(u32 mip) const { return std::max(1u, height >> mip); }
    // Sizes (bytes) of the compressed payload: latents at their bit depth, FP16 network, BC fallback.
    [[nodiscard]] u64 latentBytes() const;
    [[nodiscard]] u64 networkBytes() const;
    [[nodiscard]] u64 fallbackBytes() const;
    [[nodiscard]] u64 payloadBytes() const { return latentBytes() + networkBytes() + fallbackBytes(); }
    // GPU memory in InferenceOnSample mode (latents as B4G4R4A4 / RGBA8 2D arrays, weights, fallback textures).
    [[nodiscard]] u64 inferenceVramBytes() const;
    // Bits per texel of mip 0 over all channels (latents + network, without fallback).
    [[nodiscard]] f64 bitsPerTexel() const;
};

struct NtcSourceImages {
    Image albedo;   // sRGB encoded colour (+ alpha)
    Image normal;   // tangent-space normal encoded n*0.5+0.5 (xy used)
    Image orm;      // R occlusion, G roughness, B metallic (linear)
    Image emissive; // sRGB encoded
};

using NtcProgress = std::function<void(f32 fraction, f32 loss)>;

// Trains latents + decoder. Deterministic for a given seed and settings (independent of the thread count).
[[nodiscard]] Result<NeuralTextureSet> encodeNeuralTextures(const NtcSourceImages& sources,
                                                            const NeuralTextureSettings& settings,
                                                            const NtcProgress& progress = {});
// Resolved parameters for a preset (stride, features, format, hidden) and its estimated bits per texel.
[[nodiscard]] NeuralTextureSettings resolveNtcSettings(const NeuralTextureSettings& settings);
[[nodiscard]] f64 estimateNtcBitsPerTexel(const NeuralTextureSettings& resolved);

// Network input vector (kNtcInputs floats) of texel (x, y) of `mip` — the exact computation of mlp.glsl / ntc.glsl.
void ntcNetworkInput(const NeuralTextureSet& set, u32 mip, u32 x, u32 y, f32* input);
// CPU decode of one neural mip: channel images (all values clamped to [0,1]); index = output texture
// (0 albedo RGBA (alpha 1 when absent), 1 normal (B reconstructed), 2 ORM, 3 emissive); absent ones are empty.
[[nodiscard]] std::array<Image, 4> decodeNeuralMip(const NeuralTextureSet& set, u32 mip, u32 threads = 0);

struct NtcDecodedTextures {
    std::array<TextureData, 4> textures; // albedo BC7 sRGB, normal BC5, ORM BC7, emissive BC7 sRGB (Unknown = absent)
    f64 decodeMs = 0.0;
    f64 encodeMs = 0.0;
};
// DecodeOnLoad on the CPU: network → RGBA → BC7 / BC5 for the neural mips + the stored fallback tail.
// `bcQuality`: UASTC level for BC7 (0 fastest).
[[nodiscard]] NtcDecodedTextures decodeToTextures(const NeuralTextureSet& set, u32 bcQuality = 0, u32 threads = 0);
// Uses already decoded mips (e.g. from the GPU decoder) instead of the CPU network.
[[nodiscard]] NtcDecodedTextures encodeDecodedMips(const NeuralTextureSet& set,
                                                   const std::vector<std::array<Image, 4>>& neuralMips,
                                                   u32 bcQuality = 0);

// Quality of a decoded set against its sources (mip 0, per channel), plus the BC7/BC5 baseline when `withBaseline`.
[[nodiscard]] NtcStats evaluateNeuralTextures(const NeuralTextureSet& set, const NtcSourceImages& sources,
                                              bool withBaseline = true, u32 threads = 0);
// SSIM (8×8 windows, stride 4) of one channel of two equally sized images, values in [0,1].
[[nodiscard]] f64 computeSsim(const Image& a, const Image& b, u32 channel);
[[nodiscard]] f64 computePsnrChannel(const Image& a, const Image& b, u32 channel);

// .oxntc: "OXNT" container (latents at their bit depth, FP16 network, fallback .oxtex blobs, stats JSON).
[[nodiscard]] std::vector<std::byte> serializeNeuralTextureSet(const NeuralTextureSet& set);
[[nodiscard]] Result<NeuralTextureSet> deserializeNeuralTextureSet(std::span<const std::byte> data);

[[nodiscard]] std::string_view ntcPresetName(NtcPreset preset);

} // namespace ox::assets
