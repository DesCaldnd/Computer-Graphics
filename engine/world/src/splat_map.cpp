#include <oxwald/world/noise.hpp>
#include <oxwald/world/splat_map.hpp>

#include <oxwald/core/assert.hpp>

#include <glm/common.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>

namespace ox::world {

SplatMap::SplatMap(u32 resolution, u32 layerCount, glm::vec2 origin, f32 worldSize)
    : m_resolution(resolution), m_layers(std::min(layerCount, kMaxSplatLayers)), m_origin(origin), m_worldSize(worldSize) {
    OX_ASSERT(resolution >= 2 && layerCount >= 1 && layerCount <= kMaxSplatLayers, "invalid splat map");
    m_data.assign(usize(resolution) * resolution * kMaxSplatLayers, 0);
    fill(0);
}

std::array<f32, kMaxSplatLayers> SplatMap::weights(u32 x, u32 z) const {
    std::array<f32, kMaxSplatLayers> w{};
    f32 sum = 0.f;
    for (u32 l = 0; l < m_layers; ++l) {
        w[l] = f32(weight(x, z, l));
        sum += w[l];
    }
    if (sum > 0.f) {
        for (f32& v : w) {
            v /= sum;
        }
    }
    return w;
}

void SplatMap::setWeights(u32 x, u32 z, const std::array<f32, kMaxSplatLayers>& w) {
    f32 sum = 0.f;
    for (u32 l = 0; l < m_layers; ++l) {
        sum += std::max(0.f, w[l]);
    }
    if (sum <= 0.f) {
        return;
    }
    // Largest-remainder rounding so the u8 weights sum to exactly 255.
    std::array<i32, kMaxSplatLayers> q{};
    std::array<f32, kMaxSplatLayers> rem{};
    i32 total = 0;
    for (u32 l = 0; l < m_layers; ++l) {
        const f32 v = std::max(0.f, w[l]) / sum * 255.f;
        q[l] = i32(std::floor(v));
        rem[l] = v - f32(q[l]);
        total += q[l];
    }
    while (total < 255) {
        u32 best = 0;
        for (u32 l = 1; l < m_layers; ++l) {
            if (rem[l] > rem[best]) {
                best = l;
            }
        }
        ++q[best];
        rem[best] = -1.f;
        ++total;
    }
    for (u32 l = 0; l < m_layers; ++l) {
        setWeight(x, z, l, u8(q[l]));
    }
}

f32 SplatMap::sampleLayer(glm::vec2 w, u32 layer) const {
    const f32 r = f32(m_resolution - 1);
    const glm::vec2 t = glm::clamp(worldToTexel(w), glm::vec2(0.f), glm::vec2(r));
    const u32 x0 = std::min(u32(t.x), m_resolution - 2), z0 = std::min(u32(t.y), m_resolution - 2);
    const f32 fx = t.x - f32(x0), fz = t.y - f32(z0);
    const f32 a = glm::mix(f32(weight(x0, z0, layer)), f32(weight(x0 + 1, z0, layer)), fx);
    const f32 b = glm::mix(f32(weight(x0, z0 + 1, layer)), f32(weight(x0 + 1, z0 + 1, layer)), fx);
    return glm::mix(a, b, fz) / 255.f;
}

u32 SplatMap::dominantLayer(glm::vec2 w) const {
    u32 best = 0;
    f32 bestW = -1.f;
    for (u32 l = 0; l < m_layers; ++l) {
        const f32 v = sampleLayer(w, l);
        if (v > bestW) {
            bestW = v;
            best = l;
        }
    }
    return best;
}

void SplatMap::fill(u32 layer) {
    std::fill(m_data.begin(), m_data.end(), u8(0));
    for (usize i = 0; i < usize(m_resolution) * m_resolution; ++i) {
        m_data[i * kMaxSplatLayers + layer] = 255;
    }
}

void SplatMap::normalize(const IRect& rect) {
    const IRect r = rect.intersected(fullRect());
    for (i32 z = r.z0; z < r.z1; ++z) {
        for (i32 x = r.x0; x < r.x1; ++x) {
            setWeights(u32(x), u32(z), weights(u32(x), u32(z)));
        }
    }
}

std::vector<u8> SplatMap::packRgba8(u32 textureIndex, const IRect& rect) const {
    const IRect r = rect.intersected(fullRect());
    std::vector<u8> out;
    out.reserve(usize(r.width()) * usize(r.height()) * 4);
    for (i32 z = r.z0; z < r.z1; ++z) {
        for (i32 x = r.x0; x < r.x1; ++x) {
            const usize base = (usize(z) * m_resolution + usize(x)) * kMaxSplatLayers + textureIndex * 4;
            out.insert(out.end(), m_data.begin() + std::ptrdiff_t(base), m_data.begin() + std::ptrdiff_t(base + 4));
        }
    }
    return out;
}

namespace {
// 1 inside [lo, hi], smooth falloff to 0 over `blend` outside.
f32 band(f32 v, f32 lo, f32 hi, f32 blend) {
    if (v >= lo && v <= hi) {
        return 1.f;
    }
    if (blend <= 0.f) {
        return 0.f;
    }
    const f32 d = v < lo ? lo - v : v - hi;
    const f32 t = glm::clamp(1.f - d / blend, 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}
} // namespace

IRect autoPaint(SplatMap& splat, const Heightfield& hf, std::span<const SplatRule> rules, IRect rect) {
    const IRect r = (rect.empty() ? splat.fullRect() : rect).intersected(splat.fullRect());
    std::vector<Noise2D> noises;
    noises.reserve(rules.size());
    for (const auto& rule : rules) {
        noises.emplace_back(rule.noiseSeed);
    }
    for (i32 z = r.z0; z < r.z1; ++z) {
        for (i32 x = r.x0; x < r.x1; ++x) {
            const glm::vec2 w = splat.origin() + glm::vec2(f32(x), f32(z)) * splat.texelSize();
            const f32 h = hf.sampleHeight(w);
            const f32 slope = glm::degrees(hf.sampleSlope(w));
            std::array<f32, kMaxSplatLayers> ws{};
            ws[0] = 1.f;
            for (usize i = 0; i < rules.size(); ++i) {
                const SplatRule& rule = rules[i];
                if (rule.layer >= splat.layerCount()) {
                    continue;
                }
                f32 a = rule.strength * band(h, rule.minHeight, rule.maxHeight, rule.heightBlend) *
                        band(slope, rule.minSlopeDeg, rule.maxSlopeDeg, rule.slopeBlendDeg);
                if (rule.noiseAmount > 0.f) {
                    a *= glm::clamp(1.f - rule.noiseAmount * (0.5f + 0.5f * noises[i].simplex(w * rule.noiseFrequency)), 0.f, 1.f);
                }
                a = glm::clamp(a, 0.f, 1.f);
                for (f32& v : ws) {
                    v *= 1.f - a;
                }
                ws[rule.layer] += a;
            }
            splat.setWeights(u32(x), u32(z), ws);
        }
    }
    return r;
}

} // namespace ox::world
