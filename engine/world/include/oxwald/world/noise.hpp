#pragma once

#include <oxwald/core/types.hpp>

#include <glm/vec2.hpp>

#include <array>

namespace ox::world {

// Deterministic integer hashing helpers (stable across platforms; used for seeds of chunks/instances).
u32 hash32(u32 x);
u32 hashCombine(u32 seed, u32 v);
inline u32 hashCoord(i32 x, i32 z, u32 seed) { return hashCombine(hashCombine(seed, u32(x)), u32(z)); }
// Uniform float in [0,1) from a hash.
inline f32 hashToUnit(u32 h) { return f32(h >> 8) * (1.f / 16777216.f); }

// Small fast PRNG (PCG32) — deterministic, copyable, no std::random distribution differences between
// standard libraries (matters for reproducible procedural content).
class Rng {
public:
    explicit Rng(u64 seed = 0x853c49e6748fea9bULL, u64 stream = 0xda3e39cb94b95bdbULL);
    u32 nextU32();
    f32 nextFloat();                       // [0,1)
    f32 range(f32 lo, f32 hi) { return lo + (hi - lo) * nextFloat(); }
    u32 below(u32 n) { return n ? nextU32() % n : 0; } // slight modulo bias is fine for content
private:
    u64 m_state = 0;
    u64 m_inc = 0;
};

// Seeded 2D gradient noise. Output range is approximately [-1, 1].
class Noise2D {
public:
    explicit Noise2D(u32 seed = 0);
    [[nodiscard]] f32 perlin(glm::vec2 p) const;
    [[nodiscard]] f32 simplex(glm::vec2 p) const;
    [[nodiscard]] u32 seed() const { return m_seed; }

private:
    std::array<u8, 512> m_perm{};
    u32 m_seed = 0;
};

enum class NoiseBasis : u8 { Perlin, Simplex };
enum class FractalType : u8 {
    Fbm,    // sum of octaves, [-1,1]-ish → remapped to [0,1]
    Ridged, // 1-|n| squared and weighted by previous octave (Musgrave ridged multifractal)
    Billow, // |n|
};

struct FractalSettings {
    NoiseBasis basis = NoiseBasis::Simplex;
    FractalType type = FractalType::Fbm;
    u32 seed = 1337;
    f32 frequency = 1.f / 512.f; // cycles per world metre (first octave)
    u32 octaves = 6;
    f32 lacunarity = 2.f;
    f32 gain = 0.5f;
    glm::vec2 offset{0.f};
    // Domain warping (Inigo Quilez): p += warpStrength * (fbm(p*warpFrequency), fbm(p*warpFrequency + c)).
    f32 warpStrength = 0.f;      // metres
    f32 warpFrequency = 1.f / 256.f;
    u32 warpOctaves = 3;
};

// Fractal noise evaluator; value in [0,1] (approximately, not hard-clamped).
class FractalNoise {
public:
    explicit FractalNoise(const FractalSettings& s);
    [[nodiscard]] f32 sample(glm::vec2 worldXZ) const;
    [[nodiscard]] const FractalSettings& settings() const { return m_settings; }

private:
    [[nodiscard]] f32 basis(glm::vec2 p, const Noise2D& n) const;
    [[nodiscard]] f32 fractal(glm::vec2 p, f32 freq, u32 octaves, const Noise2D& n) const;
    FractalSettings m_settings;
    Noise2D m_noise;
    Noise2D m_warpX;
    Noise2D m_warpZ;
};

} // namespace ox::world
