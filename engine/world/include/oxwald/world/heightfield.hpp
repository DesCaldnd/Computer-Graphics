#pragma once

#include <oxwald/core/types.hpp>
#include <oxwald/world/common.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ox::world {

enum class HeightFormat : u8 {
    Float32, // 4 bytes/sample, unclamped normalized values (editing headroom)
    UNorm16, // 2 bytes/sample, normalized [0,1] quantised to 65535 steps (R16_UNORM on the GPU)
};

struct HeightfieldDesc {
    u32 resolution = 1025;      // samples per side (square). (resolution-1) quads per side.
    f32 worldSize = 1024.f;     // metres covered by the samples along X and Z
    f32 heightScale = 256.f;    // world height = heightOffset + normalized * heightScale
    f32 heightOffset = 0.f;
    glm::vec2 origin{0.f};      // world XZ of sample (0,0) — the minimum corner
    HeightFormat format = HeightFormat::Float32;
};

// Square grid of height samples. Row-major: index = z * resolution + x; +X east, +Z south (world axes).
// Values are stored *normalized* (world = heightOffset + h * heightScale) so the same data can be
// uploaded as R16_UNORM/R32_SFLOAT and rescaled without touching samples.
class Heightfield {
public:
    Heightfield() = default;
    explicit Heightfield(const HeightfieldDesc& desc);

    [[nodiscard]] const HeightfieldDesc& desc() const { return m_desc; }
    [[nodiscard]] u32 resolution() const { return m_desc.resolution; }
    [[nodiscard]] f32 spacing() const { return m_desc.worldSize / f32(m_desc.resolution - 1); }
    [[nodiscard]] bool valid() const { return m_desc.resolution >= 2; }
    [[nodiscard]] IRect fullRect() const { return {0, 0, i32(m_desc.resolution), i32(m_desc.resolution)}; }
    void setScale(f32 heightScale, f32 heightOffset);
    void setOrigin(glm::vec2 origin) { m_desc.origin = origin; }

    // --- raw sample access (normalized) ---
    [[nodiscard]] f32 normalized(u32 x, u32 z) const;
    void setNormalized(u32 x, u32 z, f32 v);
    // World height of a sample in metres; coordinates are clamped to the grid.
    [[nodiscard]] f32 heightAtSample(i32 x, i32 z) const;
    void setHeightAtSample(u32 x, u32 z, f32 worldHeight);

    // --- world-space sampling ---
    [[nodiscard]] glm::vec2 worldToSample(glm::vec2 worldXZ) const; // continuous sample coords
    [[nodiscard]] glm::vec2 sampleToWorld(glm::vec2 sampleXZ) const;
    [[nodiscard]] glm::vec3 samplePosition(u32 x, u32 z) const;     // world position of a sample
    [[nodiscard]] bool containsWorld(glm::vec2 worldXZ) const;
    [[nodiscard]] f32 sampleHeight(glm::vec2 worldXZ) const;        // bilinear, clamped at the borders
    [[nodiscard]] glm::vec3 sampleNormal(glm::vec2 worldXZ) const;  // central differences of the bilinear surface
    [[nodiscard]] f32 sampleSlope(glm::vec2 worldXZ) const;         // radians from horizontal, [0, pi/2)
    [[nodiscard]] glm::vec3 sampleNormalAtSample(u32 x, u32 z) const;
    [[nodiscard]] f32 minHeight() const; // world metres, over all samples
    [[nodiscard]] f32 maxHeight() const;
    void minMaxInRect(const IRect& r, f32& outMin, f32& outMax) const; // world metres, samples in r

    // --- holes (per sample; a quad is a hole if any of its 4 corners is) ---
    [[nodiscard]] bool hasHoles() const { return !m_holes.empty(); }
    [[nodiscard]] bool isHole(u32 x, u32 z) const;
    void setHole(u32 x, u32 z, bool hole);
    void clearHoles() { m_holes.clear(); }
    [[nodiscard]] std::span<const u8> holeMask() const { return m_holes; } // 0/1 per sample, empty if none

    // --- bulk data ---
    [[nodiscard]] std::vector<f32> toNormalizedFloats() const;
    void fromNormalizedFloats(std::span<const f32> values); // size must equal resolution^2
    [[nodiscard]] std::vector<f32> worldHeights(const IRect& rect) const; // row-major inside rect
    // Raw storage, for GPU upload: f32 or u16 per sample according to desc().format.
    [[nodiscard]] std::span<const u8> rawBytes() const;
    [[nodiscard]] std::span<u8> rawBytesMutable();
    [[nodiscard]] usize bytesPerSample() const { return m_desc.format == HeightFormat::Float32 ? 4 : 2; }
    // Copy a sub-rectangle as R16_UNORM (for GPU dirty-rect uploads regardless of storage format).
    [[nodiscard]] std::vector<u16> extractR16(const IRect& rect) const;
    [[nodiscard]] Heightfield extractTile(const IRect& rect) const; // origin adjusted; rect inclusive of the shared edge if desired
    void convertFormat(HeightFormat format);

    // --- import / export ---
    struct ImportOptions {
        f32 worldSize = 1024.f;
        f32 heightScale = 256.f;
        f32 heightOffset = 0.f;
        glm::vec2 origin{0.f};
        HeightFormat format = HeightFormat::Float32;
        bool flipZ = false; // image row 0 is +Z rather than -Z
    };
    // 16-bit (or 8-bit) grayscale PNG, R16 little-endian RAW (square, size inferred), or EXR (first
    // channel, interpreted as metres: normalized = (v - heightOffset) / heightScale).
    static std::optional<Heightfield> loadPng(const std::filesystem::path& path, const ImportOptions& opts, std::string* error = nullptr);
    static std::optional<Heightfield> loadRaw16(const std::filesystem::path& path, const ImportOptions& opts, std::string* error = nullptr);
    static std::optional<Heightfield> loadExr(const std::filesystem::path& path, const ImportOptions& opts, std::string* error = nullptr);
    // Dispatches by extension (.png, .r16/.raw, .exr).
    static std::optional<Heightfield> load(const std::filesystem::path& path, const ImportOptions& opts, std::string* error = nullptr);
    bool savePng16(const std::filesystem::path& path) const; // normalized clamped to [0,1]
    bool saveRaw16(const std::filesystem::path& path) const;

private:
    [[nodiscard]] usize index(u32 x, u32 z) const { return usize(z) * m_desc.resolution + x; }
    HeightfieldDesc m_desc{};
    std::vector<f32> m_f32;
    std::vector<u16> m_u16;
    std::vector<u8> m_holes;
};

} // namespace ox::world
