#pragma once

#include <oxwald/world/heightfield.hpp>

namespace ox::world {

class SplatMap;

enum class BrushOp : u8 { Raise, Lower, Smooth, Flatten, Noise, SetHole, ClearHole };
enum class BrushFalloff : u8 { Constant, Linear, Smooth, Spherical };

struct BrushSettings {
    BrushOp op = BrushOp::Raise;
    f32 radius = 8.f;        // metres
    f32 strength = 1.f;      // Raise/Lower/Noise: metres per application (× dt); Smooth/Flatten: blend 0..1 (× dt)
    BrushFalloff falloff = BrushFalloff::Smooth;
    f32 hardness = 0.f;      // [0,1): fraction of the radius with full strength before falloff starts
    f32 targetHeight = 0.f;  // Flatten, world metres
    f32 noiseFrequency = 0.1f; // Noise, cycles per metre
    u32 noiseSeed = 7;
};

// Falloff weight in [0,1] for a normalised distance t = dist / radius.
f32 brushWeight(const BrushSettings& b, f32 t);

// Apply a brush stroke centred at world XZ; dt scales the strength (so strokes are frame-rate
// independent). Returns the dirty rect in sample coordinates (empty if nothing changed) — push it to
// the GPU heightmap (Heightfield::extractR16) and rebuild physics tiles/min-max tree overlapping it.
IRect applyBrush(Heightfield& hf, glm::vec2 centerXZ, const BrushSettings& b, f32 dt = 1.f);

// Paint a splat layer with the same falloff model (strength = weight added per application).
IRect paintSplat(SplatMap& splat, glm::vec2 centerXZ, u32 layer, const BrushSettings& b, f32 dt = 1.f);

} // namespace ox::world
