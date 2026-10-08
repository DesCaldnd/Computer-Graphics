#pragma once

#include <oxwald/core/types.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <compare>
#include <functional>
#include <string>
#include <vector>

namespace ox::audio {

// Loaded/generated sound data (shared by every instance that plays it).
struct SoundId {
    u32 value = 0;
    [[nodiscard]] bool valid() const { return value != 0; }
    explicit operator bool() const { return valid(); }
    auto operator<=>(const SoundId&) const = default;
};

// A playing instance. Generational: becomes invalid once the voice finished, was stopped or stolen.
struct SoundHandle {
    u32 index = ~0u;
    u32 generation = 0;
    [[nodiscard]] bool valid() const { return index != ~0u; }
    explicit operator bool() const { return valid(); }
    auto operator<=>(const SoundHandle&) const = default;
};

enum class LoadMode : u8 {
    Decode, // fully decoded into memory once (shared by all instances) — SFX
    Stream, // decoded on the fly page by page — music, long ambiences
};

enum class NoiseType : u8 { White, Pink, Brownian };

enum class AttenuationModel : u8 {
    None,
    Inverse,     // minD / (minD + rolloff * (d - minD))           (OpenAL inverse clamped)
    Linear,      // 1 - rolloff * (d - minD) / (maxD - minD)
    Exponential, // (d / minD) ^ -rolloff
    Custom,      // piecewise-linear `customCurve` (distance → gain)
};

struct CurvePoint {
    f32 distance = 0.f;
    f32 gain = 1.f;
};

// 3D parameters of a sound instance. `enabled = false` → plain 2D sound (only `PlayParams::pan` applies).
struct Spatial3D {
    bool enabled = false;
    glm::vec3 position{0.f};
    glm::vec3 velocity{0.f};
    glm::vec3 direction{0.f, 0.f, -1.f}; // used by the cone

    AttenuationModel attenuation = AttenuationModel::Inverse;
    f32 minDistance = 1.f;
    f32 maxDistance = 100.f;
    f32 rolloff = 1.f;
    std::vector<CurvePoint> customCurve; // sorted by distance

    // Cone in radians (full angles). Default = omnidirectional.
    f32 coneInnerAngle = 6.2831853f;
    f32 coneOuterAngle = 6.2831853f;
    f32 coneOuterGain = 1.f;

    f32 dopplerFactor = 1.f;

    // Air absorption: low-pass cutoff falls from nearCutoff (at minDistance) to farCutoff (at maxDistance).
    bool distanceLowPass = false;
    f32 lowPassNearCutoff = 20000.f;
    f32 lowPassFarCutoff = 2000.f;

    // Query the engine's IAudioOcclusionProvider for this source.
    bool occlusion = false;
};

struct PlayParams {
    std::string bus = "SFX";
    f32 volume = 1.f;
    f32 pitch = 1.f;
    f32 pan = 0.f; // -1 left .. +1 right, 2D sounds only
    bool loop = false;
    f32 fadeInSeconds = 0.f;
    f32 startDelaySeconds = 0.f;
    i32 priority = 0; // higher wins when the voice limit is reached
    bool startPaused = false;
    Spatial3D spatial;
};

struct ListenerState {
    glm::vec3 position{0.f};
    glm::quat orientation{1.f, 0.f, 0.f, 0.f}; // identity looks down -Z, Y up
    glm::vec3 velocity{0.f};
};

// Debug visualisation sink: line segment from → to with RGBA colour.
using DebugLineFn = std::function<void(glm::vec3, glm::vec3, glm::vec4)>;

// Distance gain for a model (same formulas as the mixer uses). Custom → piecewise linear curve.
[[nodiscard]] f32 evaluateAttenuation(const Spatial3D& spatial, f32 distance);

[[nodiscard]] inline f32 dbToLinear(f32 db) { return std::pow(10.f, db / 20.f); }
[[nodiscard]] inline f32 linearToDb(f32 linear) { return 20.f * std::log10(std::max(linear, 1e-6f)); }

} // namespace ox::audio
