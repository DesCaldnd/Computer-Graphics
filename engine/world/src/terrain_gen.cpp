#include <oxwald/world/terrain_gen.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace ox::world {

void generateNoise(Heightfield& hf, const TerrainNoiseSettings& s) {
    const FractalNoise noise(s.fractal);
    const u32 res = hf.resolution();
    std::vector<f32> v(usize(res) * res);
    for (u32 z = 0; z < res; ++z) {
        for (u32 x = 0; x < res; ++x) {
            const glm::vec2 w = hf.sampleToWorld(glm::vec2(f32(x), f32(z)));
            f32 h = glm::clamp(noise.sample(w), 0.f, 1.f);
            if (s.exponent != 1.f) {
                h = std::pow(h, s.exponent);
            }
            v[usize(z) * res + x] = h;
        }
    }
    if (s.normalizeRange) {
        const auto [lo, hi] = std::minmax_element(v.begin(), v.end());
        const f32 a = *lo, b = *hi;
        const f32 inv = b > a ? 1.f / (b - a) : 0.f;
        for (f32& h : v) {
            h = (h - a) * inv;
        }
    }
    hf.fromNormalizedFloats(v);
}

void addNoise(Heightfield& hf, const FractalSettings& s, f32 amplitude) {
    const FractalNoise noise(s);
    const u32 res = hf.resolution();
    for (u32 z = 0; z < res; ++z) {
        for (u32 x = 0; x < res; ++x) {
            const glm::vec2 w = hf.sampleToWorld(glm::vec2(f32(x), f32(z)));
            hf.setNormalized(x, z, hf.normalized(x, z) + amplitude * (noise.sample(w) - 0.5f));
        }
    }
}

namespace {

// Working copy of the heights. `unit` metres map to 1.0 (thermal: the sample spacing, so slopes are
// geometric; hydraulic: the relief, matching the scale the droplet parameters are tuned for).
struct WorkMap {
    u32 res = 0;
    f32 unit = 1.f;
    f32 base = 0.f;
    std::vector<f32> h;
    std::vector<u8> fixed; // holes: not modified

    WorkMap(const Heightfield& hf, f32 unitMetres, f32 baseMetres) : res(hf.resolution()), unit(unitMetres), base(baseMetres) {
        h.resize(usize(res) * res);
        const f32 inv = 1.f / unit;
        for (u32 z = 0; z < res; ++z) {
            for (u32 x = 0; x < res; ++x) {
                h[usize(z) * res + x] = (hf.heightAtSample(i32(x), i32(z)) - base) * inv;
            }
        }
        if (hf.hasHoles()) {
            fixed.assign(hf.holeMask().begin(), hf.holeMask().end());
        }
    }
    void writeBack(Heightfield& hf) const {
        for (u32 z = 0; z < res; ++z) {
            for (u32 x = 0; x < res; ++x) {
                const usize i = usize(z) * res + x;
                if (fixed.empty() || !fixed[i]) {
                    hf.setHeightAtSample(x, z, base + h[i] * unit);
                }
            }
        }
    }
    void add(usize i, f32 d) {
        if (fixed.empty() || !fixed[i]) {
            h[i] += d;
        }
    }
};

struct HeightGrad {
    f32 height, gx, gz;
};

HeightGrad heightAndGradient(const WorkMap& m, f32 px, f32 pz) {
    const u32 x = u32(px), z = u32(pz);
    const f32 u = px - f32(x), v = pz - f32(z);
    const usize i = usize(z) * m.res + x;
    const f32 nw = m.h[i], ne = m.h[i + 1], sw = m.h[i + m.res], se = m.h[i + m.res + 1];
    return {nw * (1 - u) * (1 - v) + ne * u * (1 - v) + sw * (1 - u) * v + se * u * v,
            (ne - nw) * (1 - v) + (se - sw) * v, (sw - nw) * (1 - u) + (se - ne) * u};
}

} // namespace

IRect erodeHydraulic(Heightfield& hf, const HydraulicErosionSettings& s) {
    f32 lo = 0.f, hi = 0.f;
    hf.minMaxInRect(hf.fullRect(), lo, hi);
    WorkMap m(hf, std::max(hi - lo, 1e-3f), lo);
    const u32 res = m.res;
    if (res < 4) {
        return {};
    }
    // Erosion brush: offsets within radius, weights (r - d) normalised.
    const i32 r = i32(std::max(1u, s.erosionRadius));
    std::vector<std::pair<glm::ivec2, f32>> brush;
    f32 wsum = 0.f;
    for (i32 dz = -r; dz <= r; ++dz) {
        for (i32 dx = -r; dx <= r; ++dx) {
            const f32 d = std::sqrt(f32(dx * dx + dz * dz));
            if (d < f32(r)) {
                brush.push_back({{dx, dz}, f32(r) - d});
                wsum += f32(r) - d;
            }
        }
    }
    for (auto& b : brush) {
        b.second /= wsum;
    }

    auto deposit = [&](f32 px, f32 pz, f32 amount) {
        const u32 x = u32(px), z = u32(pz);
        const f32 u = px - f32(x), v = pz - f32(z);
        const usize i = usize(z) * res + x;
        m.add(i, amount * (1 - u) * (1 - v));
        m.add(i + 1, amount * u * (1 - v));
        m.add(i + res, amount * (1 - u) * v);
        m.add(i + res + 1, amount * u * v);
    };

    Rng rng(s.seed, 0x1234567ULL);
    const f32 maxPos = f32(res - 1) - 1e-3f;
    for (u32 d = 0; d < s.droplets; ++d) {
        f32 px = rng.nextFloat() * maxPos, pz = rng.nextFloat() * maxPos;
        f32 dirX = 0.f, dirZ = 0.f, speed = s.initialSpeed, water = s.initialWater, sediment = 0.f;
        bool alive = true;
        for (u32 life = 0; life < s.maxLifetime; ++life) {
            const u32 nx = u32(px), nz = u32(pz);
            const f32 ox = px - f32(nx), oz = pz - f32(nz);
            const HeightGrad hg = heightAndGradient(m, px, pz);
            dirX = dirX * s.inertia - hg.gx * (1 - s.inertia);
            dirZ = dirZ * s.inertia - hg.gz * (1 - s.inertia);
            const f32 len = std::sqrt(dirX * dirX + dirZ * dirZ);
            if (len < 1e-8f) {
                break; // flat: droplet stops, remainder deposited below
            }
            dirX /= len;
            dirZ /= len;
            const f32 oldX = px, oldZ = pz;
            px += dirX;
            pz += dirZ;
            if (px < 0.f || pz < 0.f || px >= maxPos || pz >= maxPos) {
                px = oldX;
                pz = oldZ;
                alive = !s.loseSedimentAtBorder;
                break;
            }
            const f32 deltaH = heightAndGradient(m, px, pz).height - hg.height;
            const f32 capacity = std::max(-deltaH, s.minSlope) * speed * water * s.sedimentCapacity;
            if (sediment > capacity || deltaH > 0.f) {
                const f32 amount = deltaH > 0.f ? std::min(deltaH, sediment) : (sediment - capacity) * s.depositSpeed;
                sediment -= amount;
                deposit(oldX, oldZ, amount);
            } else {
                const f32 amount = std::min((capacity - sediment) * s.erodeSpeed, -deltaH);
                f32 removed = 0.f;
                for (const auto& [off, w] : brush) {
                    const i32 bx = i32(nx) + off.x, bz = i32(nz) + off.y;
                    if (bx < 0 || bz < 0 || bx >= i32(res) || bz >= i32(res)) {
                        continue;
                    }
                    const usize bi = usize(bz) * res + usize(bx);
                    if (!m.fixed.empty() && m.fixed[bi]) {
                        continue;
                    }
                    m.h[bi] -= amount * w;
                    removed += amount * w;
                }
                sediment += removed;
            }
            speed = std::sqrt(std::max(0.f, speed * speed - deltaH * s.gravity));
            water *= (1.f - s.evaporateSpeed);
            (void)ox;
            (void)oz;
        }
        if (alive && s.depositRemainder && sediment > 0.f) {
            deposit(std::min(px, maxPos), std::min(pz, maxPos), sediment);
        }
    }
    m.writeBack(hf);
    return hf.fullRect();
}

IRect erodeThermal(Heightfield& hf, const ThermalErosionSettings& s) {
    WorkMap m(hf, hf.spacing(), 0.f);
    const u32 res = m.res;
    const f32 talus = std::tan(glm::radians(s.talusAngleDeg)); // in sample units: height diff per sample
    std::vector<f32> delta(m.h.size());
    constexpr i32 kOff[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (u32 it = 0; it < s.iterations; ++it) {
        std::fill(delta.begin(), delta.end(), 0.f);
        for (u32 z = 0; z < res; ++z) {
            for (u32 x = 0; x < res; ++x) {
                const usize i = usize(z) * res + x;
                if (!m.fixed.empty() && m.fixed[i]) {
                    continue;
                }
                f32 diffs[4] = {};
                f32 total = 0.f, maxD = 0.f;
                for (int k = 0; k < 4; ++k) {
                    const i32 nx = i32(x) + kOff[k][0], nz = i32(z) + kOff[k][1];
                    if (nx < 0 || nz < 0 || nx >= i32(res) || nz >= i32(res)) {
                        continue;
                    }
                    const usize j = usize(nz) * res + usize(nx);
                    if (!m.fixed.empty() && m.fixed[j]) {
                        continue;
                    }
                    const f32 d = m.h[i] - m.h[j];
                    if (d > talus) {
                        diffs[k] = d - talus;
                        total += diffs[k];
                        maxD = std::max(maxD, diffs[k]);
                    }
                }
                if (total <= 0.f) {
                    continue;
                }
                // Move half the largest excess (so the pair ends at the talus slope), scaled by rate.
                const f32 moved = s.rate * 0.5f * maxD;
                delta[i] -= moved;
                for (int k = 0; k < 4; ++k) {
                    if (diffs[k] > 0.f) {
                        const usize j = usize(i32(z) + kOff[k][1]) * res + usize(i32(x) + kOff[k][0]);
                        delta[j] += moved * diffs[k] / total;
                    }
                }
            }
        }
        for (usize i = 0; i < m.h.size(); ++i) {
            m.h[i] += delta[i];
        }
    }
    m.writeBack(hf);
    return hf.fullRect();
}

void terrace(Heightfield& hf, u32 steps, f32 sharpness, f32 blend) {
    if (steps == 0) {
        return;
    }
    const u32 res = hf.resolution();
    const f32 s = glm::clamp(sharpness, 0.f, 0.999f);
    for (u32 z = 0; z < res; ++z) {
        for (u32 x = 0; x < res; ++x) {
            const f32 h = hf.normalized(x, z);
            const f32 t = h * f32(steps);
            const f32 k = std::floor(t);
            const f32 f = glm::clamp((t - k - 0.5f) / (1.f - s) + 0.5f, 0.f, 1.f);
            const f32 riser = f * f * (3.f - 2.f * f);
            hf.setNormalized(x, z, glm::mix(h, (k + riser) / f32(steps), blend));
        }
    }
}

HeightStats computeStats(const Heightfield& hf) {
    HeightStats st;
    const u32 res = hf.resolution();
    const f32 sp = hf.spacing();
    st.minHeight = std::numeric_limits<f32>::max();
    st.maxHeight = std::numeric_limits<f32>::lowest();
    f32 maxGrad = 0.f;
    f64 lapSum = 0.0;
    u64 lapCount = 0;
    for (u32 z = 0; z < res; ++z) {
        for (u32 x = 0; x < res; ++x) {
            const i32 xi = i32(x), zi = i32(z);
            const f32 h = hf.heightAtSample(xi, zi);
            st.sum += h;
            st.minHeight = std::min(st.minHeight, h);
            st.maxHeight = std::max(st.maxHeight, h);
            if (x + 1 < res) {
                maxGrad = std::max(maxGrad, std::abs(hf.heightAtSample(xi + 1, zi) - h) / sp);
            }
            if (z + 1 < res) {
                maxGrad = std::max(maxGrad, std::abs(hf.heightAtSample(xi, zi + 1) - h) / sp);
            }
            if (x > 0 && z > 0 && x + 1 < res && z + 1 < res) {
                const f32 lap = hf.heightAtSample(xi + 1, zi) + hf.heightAtSample(xi - 1, zi) +
                                hf.heightAtSample(xi, zi + 1) + hf.heightAtSample(xi, zi - 1) - 4.f * h;
                lapSum += std::abs(lap);
                ++lapCount;
            }
        }
    }
    st.maxSlopeDeg = glm::degrees(std::atan(maxGrad));
    st.meanAbsLaplacian = lapCount ? f32(lapSum / f64(lapCount)) : 0.f;
    return st;
}

} // namespace ox::world
