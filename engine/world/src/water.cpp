#include <oxwald/world/noise.hpp>
#include <oxwald/world/water.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace ox::world {

namespace {
constexpr f32 kTwoPi = 6.28318530718f;
constexpr f32 kGravity = 9.81f;
} // namespace

GerstnerWaves::GerstnerWaves(std::span<const GerstnerWave> waves, f32 base) : baseHeight(base) { setWaves(waves); }

void GerstnerWaves::setWaves(std::span<const GerstnerWave> waves) {
    m_count = u32(std::min<usize>(waves.size(), kMaxGerstnerWaves));
    for (u32 i = 0; i < m_count; ++i) {
        const GerstnerWave& w = waves[i];
        const glm::vec2 d = glm::dot(w.direction, w.direction) > 1e-12f ? glm::normalize(w.direction) : glm::vec2(1.f, 0.f);
        const f32 k = kTwoPi / std::max(w.wavelength, 1e-3f);
        const f32 omega = std::sqrt(kGravity * k) * w.speedScale;
        const f32 qa = glm::clamp(w.steepness, 0.f, 1.f) / (k * f32(m_count));
        m_packed[i] = {{d.x, d.y, k, omega}, {w.amplitude, qa, w.phase, 0.f}};
    }
}

GerstnerWaves GerstnerWaves::fromWind(glm::vec2 windDir, f32 windSpeed, u32 count, u32 seed, f32 steepness, f32 spreadDeg,
                                      f32 base) {
    count = std::clamp(count, 1u, kMaxGerstnerWaves);
    const glm::vec2 d = glm::dot(windDir, windDir) > 1e-12f ? glm::normalize(windDir) : glm::vec2(1.f, 0.f);
    const f32 maxL = glm::clamp(0.4f * windSpeed * windSpeed, 2.f, 200.f);
    const f32 minL = maxL / 5.f;
    Rng rng(seed, 0x51ED2701ULL);
    std::vector<GerstnerWave> waves(count);
    for (u32 i = 0; i < count; ++i) {
        const f32 u = count > 1 ? f32(i) / f32(count - 1) : 0.f;
        GerstnerWave& w = waves[i];
        w.wavelength = maxL * std::pow(minL / maxL, u);
        const f32 ang = (rng.nextFloat() * 2.f - 1.f) * spreadDeg * (kTwoPi / 360.f);
        w.direction = {d.x * std::cos(ang) - d.y * std::sin(ang), d.x * std::sin(ang) + d.y * std::cos(ang)};
        w.amplitude = w.wavelength * 0.012f;
        w.steepness = steepness;
        w.phase = rng.nextFloat() * kTwoPi;
    }
    return GerstnerWaves(waves, base);
}

// Keep in sync with oxGerstnerDisplacement in engine/shaders/world/gerstner.glsl.
glm::vec3 GerstnerWaves::displacement(glm::vec2 x0, f32 t) const {
    glm::vec3 p(0.f);
    for (u32 i = 0; i < m_count; ++i) {
        const GerstnerWaveGpu& w = m_packed[i];
        const glm::vec2 d(w.dirK.x, w.dirK.y);
        const f32 theta = w.dirK.z * glm::dot(d, x0) - w.dirK.w * t + w.amp.z;
        const f32 c = std::cos(theta), s = std::sin(theta);
        p.x += w.amp.y * d.x * c;
        p.z += w.amp.y * d.y * c;
        p.y += w.amp.x * s;
    }
    return p;
}

// Keep in sync with oxGerstnerNormal in engine/shaders/world/gerstner.glsl.
glm::vec3 GerstnerWaves::normalAtRest(glm::vec2 x0, f32 t) const {
    glm::vec3 tx(1.f, 0.f, 0.f), tz(0.f, 0.f, 1.f); // dP/dx0, dP/dz0
    for (u32 i = 0; i < m_count; ++i) {
        const GerstnerWaveGpu& w = m_packed[i];
        const glm::vec2 d(w.dirK.x, w.dirK.y);
        const f32 k = w.dirK.z;
        const f32 theta = k * glm::dot(d, x0) - w.dirK.w * t + w.amp.z;
        const f32 c = std::cos(theta), s = std::sin(theta);
        const f32 qks = w.amp.y * k * s, akc = w.amp.x * k * c;
        tx += glm::vec3(-qks * d.x * d.x, akc * d.x, -qks * d.x * d.y);
        tz += glm::vec3(-qks * d.x * d.y, akc * d.y, -qks * d.y * d.y);
    }
    return glm::normalize(glm::cross(tz, tx));
}

glm::vec2 GerstnerWaves::findRestPoint(glm::vec2 xz, f32 t, u32 iterations) const {
    glm::vec2 x0 = xz;
    for (u32 i = 0; i < iterations; ++i) {
        const glm::vec3 d = displacement(x0, t);
        x0 = xz - glm::vec2(d.x, d.z);
    }
    return x0;
}

f32 GerstnerWaves::heightAt(glm::vec2 xz, f32 t, u32 iterations) const {
    return baseHeight + displacement(findRestPoint(xz, t, iterations), t).y;
}

glm::vec3 GerstnerWaves::normalAt(glm::vec2 xz, f32 t, u32 iterations) const {
    return normalAtRest(findRestPoint(xz, t, iterations), t);
}

GerstnerParamsGpu GerstnerWaves::toGpu(f32 time) const {
    GerstnerParamsGpu g{};
    for (u32 i = 0; i < m_count; ++i) {
        g.waves[i] = m_packed[i];
    }
    g.info = {f32(m_count), baseHeight, time, 0.f};
    return g;
}

// --- buoyancy ---------------------------------------------------------------------------------

BuoyancySettings BuoyancySettings::fromBox(glm::vec3 he, u32 n) {
    n = std::max(1u, n);
    BuoyancySettings s;
    const glm::vec3 cell = 2.f * he / f32(n);
    const f32 vol = cell.x * cell.y * cell.z;
    for (u32 z = 0; z < n; ++z) {
        for (u32 y = 0; y < n; ++y) {
            for (u32 x = 0; x < n; ++x) {
                const glm::vec3 p = -he + cell * (glm::vec3(f32(x), f32(y), f32(z)) + 0.5f);
                s.points.push_back({p, vol, cell.y});
            }
        }
    }
    return s;
}

f32 BuoyancySettings::totalVolume() const {
    f32 v = 0.f;
    for (const auto& p : points) {
        v += p.volume;
    }
    return v;
}

BuoyancyResult computeBuoyancy(const BuoyancySettings& s, glm::vec3 com, glm::quat rot, glm::vec3 linVel, glm::vec3 angVel,
                               const WaterHeightFn& waterHeight, glm::vec3 waterVel) {
    BuoyancyResult r;
    if (!waterHeight) {
        return r;
    }
    f32 total = 0.f;
    for (const BuoyancyPoint& pt : s.points) {
        total += pt.volume;
        const glm::vec3 arm = rot * pt.localPosition;
        const glm::vec3 p = com + arm;
        const f32 depth = waterHeight(glm::vec2(p.x, p.z)) - p.y;
        const f32 frac = glm::clamp(depth / std::max(pt.height, 1e-4f) + 0.5f, 0.f, 1.f);
        if (frac <= 0.f) {
            continue;
        }
        const f32 vSub = pt.volume * frac;
        const f32 displacedMass = s.fluidDensity * vSub;
        const glm::vec3 vPoint = linVel + glm::cross(angVel, arm);
        glm::vec3 f(0.f, displacedMass * s.gravity, 0.f);
        f -= s.linearDrag * displacedMass * (vPoint - waterVel);
        r.force += f;
        r.torque += glm::cross(arm, f);
        r.submergedVolume += vSub;
    }
    r.submergedFraction = total > 0.f ? r.submergedVolume / total : 0.f;
    r.torque -= s.angularDrag * s.fluidDensity * r.submergedVolume * angVel;
    return r;
}

} // namespace ox::world
