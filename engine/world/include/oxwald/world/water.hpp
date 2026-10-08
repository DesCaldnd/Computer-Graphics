#pragma once

#include <oxwald/world/common.hpp>

#include <glm/gtc/quaternion.hpp>

#include <array>
#include <functional>
#include <span>
#include <vector>

namespace ox::world {

// --- Gerstner waves ---------------------------------------------------------------------------
// Sum of N Gerstner (trochoidal) waves. The math is mirrored 1:1 in engine/shaders/world/gerstner.glsl
// using the packed GerstnerWaveGpu layout, so CPU buoyancy and GPU displacement agree.
//
// Lagrangian form (what the vertex shader does): a grid point x0 moves to
//   P(x0, t) = (x0.x + Σ qa·Dx·cosθ,  base + Σ A·sinθ,  x0.z + Σ qa·Dz·cosθ),  θ = k(D·x0) − ωt + φ
// with k = 2π/λ, ω = sqrt(g·k)·speedScale (deep water dispersion), qa = steepness / (k·N).
inline constexpr u32 kMaxGerstnerWaves = 16;

struct GerstnerWave {
    glm::vec2 direction{1.f, 0.f}; // XZ, normalised on use (direction of travel)
    f32 wavelength = 10.f;         // metres
    f32 amplitude = 0.2f;          // metres
    f32 steepness = 0.5f;          // 0 = sine wave, 1 = sharp crests (loops if the sum exceeds 1)
    f32 phase = 0.f;               // radians
    f32 speedScale = 1.f;
};

struct GerstnerWaveGpu { // 32 bytes
    glm::vec4 dirK; // xy = direction, z = k, w = omega
    glm::vec4 amp;  // x = amplitude, y = qa (horizontal amplitude), z = phase, w = 0
};

struct GerstnerParamsGpu { // std140/std430, 528 bytes
    GerstnerWaveGpu waves[kMaxGerstnerWaves];
    glm::vec4 info; // x = wave count, y = base height, z = time, w = 0
};

class GerstnerWaves {
public:
    GerstnerWaves() = default;
    explicit GerstnerWaves(std::span<const GerstnerWave> waves, f32 baseHeight = 0.f);

    // Plausible wind-driven wave set: wavelengths geometric between minWavelength and ~5 × that,
    // directions spread ±spreadDeg around the wind, amplitude ∝ wavelength (constant steepness).
    static GerstnerWaves fromWind(glm::vec2 windDirection, f32 windSpeed, u32 count, u32 seed, f32 steepness = 0.6f,
                                  f32 spreadDeg = 35.f, f32 baseHeight = 0.f);

    void setWaves(std::span<const GerstnerWave> waves);
    [[nodiscard]] std::span<const GerstnerWaveGpu> packed() const { return {m_packed.data(), m_count}; }
    [[nodiscard]] u32 count() const { return m_count; }
    f32 baseHeight = 0.f;

    // Lagrangian displacement of the rest point x0 (offset, including y height above base).
    [[nodiscard]] glm::vec3 displacement(glm::vec2 x0, f32 t) const;
    [[nodiscard]] glm::vec3 positionAt(glm::vec2 x0, f32 t) const { return glm::vec3(x0.x, baseHeight, x0.y) + displacement(x0, t); }
    // Surface normal of the displaced point P(x0, t) (exact cross product of the parametric tangents).
    [[nodiscard]] glm::vec3 normalAtRest(glm::vec2 x0, f32 t) const;
    // Eulerian queries at a world XZ (inverts the horizontal displacement by fixed-point iteration).
    [[nodiscard]] glm::vec2 findRestPoint(glm::vec2 worldXZ, f32 t, u32 iterations = 6) const;
    [[nodiscard]] f32 heightAt(glm::vec2 worldXZ, f32 t, u32 iterations = 6) const;
    [[nodiscard]] glm::vec3 normalAt(glm::vec2 worldXZ, f32 t, u32 iterations = 6) const;
    [[nodiscard]] GerstnerParamsGpu toGpu(f32 time) const;

private:
    std::array<GerstnerWaveGpu, kMaxGerstnerWaves> m_packed{};
    u32 m_count = 0;
};

// --- buoyancy ---------------------------------------------------------------------------------
// The body's volume is approximated by sample points, each standing for a small cube of `volume`
// and vertical size `height`; its submerged fraction is clamp(depth/height + 0.5, 0, 1).
struct BuoyancyPoint {
    glm::vec3 localPosition{0.f}; // relative to the centre of mass
    f32 volume = 0.f;             // m³
    f32 height = 0.1f;            // metres
};

struct BuoyancySettings {
    std::vector<BuoyancyPoint> points;
    f32 fluidDensity = 1000.f;   // kg/m³ (fresh water 1000, sea 1025)
    f32 gravity = 9.81f;
    f32 linearDrag = 1.f;        // 1/s, relative velocity drag per kg of displaced fluid
    f32 angularDrag = 0.5f;      // 1/s, angular damping per kg of displaced fluid
    // Uniform grid of n³ points filling a box (volume split evenly).
    static BuoyancySettings fromBox(glm::vec3 halfExtents, u32 subdivisions = 3);
    [[nodiscard]] f32 totalVolume() const;
};

struct BuoyancyResult {
    glm::vec3 force{0.f};  // world, apply at the centre of mass
    glm::vec3 torque{0.f}; // world, about the centre of mass
    f32 submergedVolume = 0.f;
    f32 submergedFraction = 0.f;
};

using WaterHeightFn = std::function<f32(glm::vec2 worldXZ)>;

BuoyancyResult computeBuoyancy(const BuoyancySettings& s, glm::vec3 centerOfMass, glm::quat rotation, glm::vec3 linearVelocity,
                               glm::vec3 angularVelocity, const WaterHeightFn& waterHeight,
                               glm::vec3 waterVelocity = glm::vec3(0.f));

} // namespace ox::world
