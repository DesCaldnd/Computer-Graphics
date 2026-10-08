#include <oxwald/world/noise.hpp>

#include <glm/common.hpp>

#include <cmath>

namespace ox::world {

u32 hash32(u32 x) {
    // lowbias32 (Chris Wellons)
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

u32 hashCombine(u32 seed, u32 v) { return hash32(seed ^ (v + 0x9e3779b9U + (seed << 6) + (seed >> 2))); }

Rng::Rng(u64 seed, u64 stream) {
    m_state = 0;
    m_inc = (stream << 1u) | 1u;
    nextU32();
    m_state += seed;
    nextU32();
}

u32 Rng::nextU32() {
    const u64 old = m_state;
    m_state = old * 6364136223846793005ULL + m_inc;
    const u32 xorshifted = u32(((old >> 18u) ^ old) >> 27u);
    const u32 rot = u32(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
}

f32 Rng::nextFloat() { return f32(nextU32() >> 8) * (1.f / 16777216.f); }

Noise2D::Noise2D(u32 seed) : m_seed(seed) {
    std::array<u8, 256> p{};
    for (u32 i = 0; i < 256; ++i) {
        p[i] = u8(i);
    }
    Rng rng(seed, 0x2545F4914F6CDD1DULL);
    for (u32 i = 255; i > 0; --i) {
        const u32 j = rng.below(i + 1);
        std::swap(p[i], p[j]);
    }
    for (u32 i = 0; i < 512; ++i) {
        m_perm[i] = p[i & 255];
    }
}

namespace {

inline f32 fade(f32 t) { return t * t * t * (t * (t * 6.f - 15.f) + 10.f); }

// 8 unit-ish gradient directions; cheap and isotropic enough for terrain.
inline f32 grad(u8 h, f32 x, f32 y) {
    switch (h & 7) {
    case 0: return x + y;
    case 1: return -x + y;
    case 2: return x - y;
    case 3: return -x - y;
    case 4: return x;
    case 5: return -x;
    case 6: return y;
    default: return -y;
    }
}

} // namespace

f32 Noise2D::perlin(glm::vec2 p) const {
    const f32 fx = std::floor(p.x), fy = std::floor(p.y);
    const i32 xi = i32(fx) & 255, yi = i32(fy) & 255;
    const f32 x = p.x - fx, y = p.y - fy;
    const f32 u = fade(x), v = fade(y);
    const u8 aa = m_perm[m_perm[xi] + yi], ab = m_perm[m_perm[xi] + yi + 1];
    const u8 ba = m_perm[m_perm[xi + 1] + yi], bb = m_perm[m_perm[xi + 1] + yi + 1];
    const f32 x1 = glm::mix(grad(aa, x, y), grad(ba, x - 1.f, y), u);
    const f32 x2 = glm::mix(grad(ab, x, y - 1.f), grad(bb, x - 1.f, y - 1.f), u);
    return glm::mix(x1, x2, v) * 0.7071f; // diagonal gradients have length sqrt(2)
}

f32 Noise2D::simplex(glm::vec2 p) const {
    constexpr f32 F2 = 0.36602540378f; // (sqrt(3)-1)/2
    constexpr f32 G2 = 0.2113248654f;  // (3-sqrt(3))/6
    const f32 s = (p.x + p.y) * F2;
    const f32 i = std::floor(p.x + s), j = std::floor(p.y + s);
    const f32 t = (i + j) * G2;
    const f32 x0 = p.x - (i - t), y0 = p.y - (j - t);
    const i32 i1 = x0 > y0 ? 1 : 0, j1 = x0 > y0 ? 0 : 1;
    const f32 x1 = x0 - f32(i1) + G2, y1 = y0 - f32(j1) + G2;
    const f32 x2 = x0 - 1.f + 2.f * G2, y2 = y0 - 1.f + 2.f * G2;
    const i32 ii = i32(i) & 255, jj = i32(j) & 255;
    auto corner = [&](f32 x, f32 y, u8 h) {
        f32 tt = 0.5f - x * x - y * y;
        if (tt < 0.f) {
            return 0.f;
        }
        tt *= tt;
        return tt * tt * grad(h, x, y);
    };
    const f32 n0 = corner(x0, y0, m_perm[ii + m_perm[jj]]);
    const f32 n1 = corner(x1, y1, m_perm[ii + i1 + m_perm[jj + j1]]);
    const f32 n2 = corner(x2, y2, m_perm[ii + 1 + m_perm[jj + 1]]);
    return 45.23f * (n0 + n1 + n2);
}

FractalNoise::FractalNoise(const FractalSettings& s)
    : m_settings(s), m_noise(s.seed), m_warpX(hash32(s.seed ^ 0xA5A5A5A5u)), m_warpZ(hash32(s.seed ^ 0x5A5A5A5Au)) {}

f32 FractalNoise::basis(glm::vec2 p, const Noise2D& n) const {
    return m_settings.basis == NoiseBasis::Perlin ? n.perlin(p) : n.simplex(p);
}

f32 FractalNoise::fractal(glm::vec2 p, f32 freq, u32 octaves, const Noise2D& n) const {
    f32 sum = 0.f, amp = 1.f, norm = 0.f, weight = 1.f;
    for (u32 o = 0; o < octaves; ++o) {
        // Per-octave offset avoids the lattice origin artefact where all octaves are zero.
        const glm::vec2 q = p * freq + glm::vec2(f32(o) * 17.13f, f32(o) * -9.71f);
        f32 v = basis(q, n);
        switch (m_settings.type) {
        case FractalType::Fbm: v = v * 0.5f + 0.5f; break;
        case FractalType::Billow: v = std::abs(v); break;
        case FractalType::Ridged:
            v = 1.f - std::abs(v);
            v *= v;
            v *= weight;
            weight = glm::clamp(v * 2.f, 0.f, 1.f);
            break;
        }
        sum += v * amp;
        norm += amp;
        amp *= m_settings.gain;
        freq *= m_settings.lacunarity;
    }
    return norm > 0.f ? sum / norm : 0.f;
}

f32 FractalNoise::sample(glm::vec2 xz) const {
    glm::vec2 p = xz + m_settings.offset;
    if (m_settings.warpStrength > 0.f) {
        f32 wf = m_settings.warpFrequency;
        f32 wx = 0.f, wz = 0.f, amp = 1.f, norm = 0.f;
        for (u32 o = 0; o < std::max(1u, m_settings.warpOctaves); ++o) {
            wx += m_warpX.simplex(p * wf) * amp;
            wz += m_warpZ.simplex(p * wf + glm::vec2(5.2f, 1.3f)) * amp;
            norm += amp;
            amp *= 0.5f;
            wf *= 2.f;
        }
        p += glm::vec2(wx, wz) / norm * m_settings.warpStrength;
    }
    return fractal(p, m_settings.frequency, std::max(1u, m_settings.octaves), m_noise);
}

} // namespace ox::world
