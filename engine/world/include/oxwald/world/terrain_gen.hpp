#pragma once

#include <oxwald/world/heightfield.hpp>
#include <oxwald/world/noise.hpp>

namespace ox::world {

// Fill the heightfield with fractal noise (normalized values). Noise is evaluated in world space, so
// adjacent tiles generated with the same settings match at their shared border.
struct TerrainNoiseSettings {
    FractalSettings fractal{};
    f32 exponent = 1.f;          // h = pow(h, exponent) — >1 flattens valleys, sharpens peaks
    bool normalizeRange = false; // stretch the result to exactly [0,1] (breaks tile continuity!)
};
void generateNoise(Heightfield& hf, const TerrainNoiseSettings& s);
// Add (or multiply) a noise layer on top of existing data: h += amplitude * (noise - 0.5).
void addNoise(Heightfield& hf, const FractalSettings& s, f32 amplitudeNormalized);

// Particle-based hydraulic erosion (droplets, after H. Beyer 2015 / S. Lague). Heights are normalised
// to the terrain's relief internally (parameters are tuned for that), so results do not depend on
// heightScale/heightOffset. Deterministic for a given seed. Mass-conserving unless loseSedimentAtBorder.
struct HydraulicErosionSettings {
    u32 droplets = 50000;
    u32 seed = 1;
    u32 maxLifetime = 48;
    f32 inertia = 0.05f;          // 0 = follow gradient exactly, 1 = keep direction
    f32 sedimentCapacity = 4.f;   // capacity factor
    f32 minSlope = 0.01f;         // minimum capacity slope (avoids zero capacity on flats)
    f32 erodeSpeed = 0.3f;
    f32 depositSpeed = 0.3f;
    f32 evaporateSpeed = 0.01f;
    f32 gravity = 4.f;
    f32 initialWater = 1.f;
    f32 initialSpeed = 1.f;
    u32 erosionRadius = 3;        // in samples; erosion is spread over a disc for smoother results
    bool depositRemainder = true; // drop carried sediment where a droplet dies (better mass conservation)
    bool loseSedimentAtBorder = false; // true: droplets leaving the map take their sediment with them
};
// Returns the dirty rect (whole map). Holes are left untouched.
IRect erodeHydraulic(Heightfield& hf, const HydraulicErosionSettings& s);

// Thermal erosion: material slides to lower neighbours where the slope exceeds the talus angle.
// Mass-conserving (up to float rounding) except at the map border.
struct ThermalErosionSettings {
    u32 iterations = 50;
    f32 talusAngleDeg = 35.f;
    f32 rate = 0.5f; // fraction of the excess moved per iteration (0..1)
};
IRect erodeThermal(Heightfield& hf, const ThermalErosionSettings& s);

// Terracing: quantise heights into `steps` levels with soft risers. sharpness in [0,1).
void terrace(Heightfield& hf, u32 steps, f32 sharpness, f32 blend = 1.f);

// Simple statistics used by tools/tests.
struct HeightStats {
    f64 sum = 0.0;        // sum of world heights
    f32 minHeight = 0.f;
    f32 maxHeight = 0.f;
    f32 maxSlopeDeg = 0.f; // max sample-to-neighbour slope
    f32 meanAbsLaplacian = 0.f; // roughness measure (metres)
};
HeightStats computeStats(const Heightfield& hf);

} // namespace ox::world
