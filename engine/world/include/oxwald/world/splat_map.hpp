#pragma once

#include <oxwald/world/heightfield.hpp>

#include <array>
#include <span>
#include <vector>

namespace ox::world {

inline constexpr u32 kMaxSplatLayers = 8;

// Per-texel material weights for up to 8 terrain layers, u8 each (sum normalised to 255).
// GPU layout: two RGBA8_UNORM textures — layers 0..3 in texture 0, 4..7 in texture 1 (packRgba8).
// The splat map covers the same world rect as its heightfield but may have a different resolution.
class SplatMap {
public:
    SplatMap() = default;
    SplatMap(u32 resolution, u32 layerCount, glm::vec2 origin, f32 worldSize);

    [[nodiscard]] u32 resolution() const { return m_resolution; }
    [[nodiscard]] u32 layerCount() const { return m_layers; }
    [[nodiscard]] glm::vec2 origin() const { return m_origin; }
    [[nodiscard]] f32 worldSize() const { return m_worldSize; }
    [[nodiscard]] f32 texelSize() const { return m_worldSize / f32(m_resolution - 1); }
    [[nodiscard]] IRect fullRect() const { return {0, 0, i32(m_resolution), i32(m_resolution)}; }

    [[nodiscard]] u8 weight(u32 x, u32 z, u32 layer) const { return m_data[(usize(z) * m_resolution + x) * kMaxSplatLayers + layer]; }
    void setWeight(u32 x, u32 z, u32 layer, u8 w) { m_data[(usize(z) * m_resolution + x) * kMaxSplatLayers + layer] = w; }
    [[nodiscard]] std::array<f32, kMaxSplatLayers> weights(u32 x, u32 z) const; // normalised floats
    void setWeights(u32 x, u32 z, const std::array<f32, kMaxSplatLayers>& w);   // normalises to 255
    // Bilinear world-space lookup of one layer, [0,1].
    [[nodiscard]] f32 sampleLayer(glm::vec2 worldXZ, u32 layer) const;
    [[nodiscard]] u32 dominantLayer(glm::vec2 worldXZ) const;
    [[nodiscard]] glm::vec2 worldToTexel(glm::vec2 w) const { return (w - m_origin) / texelSize(); }

    void fill(u32 layer); // 100% of `layer` everywhere
    void normalize(const IRect& rect);

    // RGBA8 texture data for texture index 0 (layers 0-3) or 1 (layers 4-7), rows inside rect.
    [[nodiscard]] std::vector<u8> packRgba8(u32 textureIndex, const IRect& rect) const;
    [[nodiscard]] std::span<const u8> raw() const { return m_data; } // 8 bytes per texel
    [[nodiscard]] std::span<u8> rawMutable() { return m_data; }

private:
    u32 m_resolution = 0;
    u32 m_layers = 0;
    glm::vec2 m_origin{0.f};
    f32 m_worldSize = 0.f;
    std::vector<u8> m_data; // texel-major, kMaxSplatLayers bytes per texel (unused layers stay 0)
};

// Auto-painting rule: layer coverage = strength * band(height) * band(slope) [* noise].
// Rules are applied in order as overlays: weights *= (1 - a); weights[layer] += a — later rules win.
struct SplatRule {
    u32 layer = 0;
    f32 minHeight = -1e9f, maxHeight = 1e9f; // world metres
    f32 heightBlend = 5.f;                   // metres of soft transition outside the band
    f32 minSlopeDeg = 0.f, maxSlopeDeg = 90.f;
    f32 slopeBlendDeg = 5.f;
    f32 strength = 1.f;
    f32 noiseAmount = 0.f;    // 0..1, breaks up transitions
    f32 noiseFrequency = 0.05f;
    u32 noiseSeed = 0;
};

// Paint `rect` (texel coords; empty = everything) of the splat map from the heightfield using rules.
IRect autoPaint(SplatMap& splat, const Heightfield& hf, std::span<const SplatRule> rules, IRect rect = {});

} // namespace ox::world
