#pragma once

#include <oxwald/animation/clip.hpp>

namespace ox::anim {

// Packed rotation: "smallest three" quantisation into 48 bits (2 bits for the dropped component's
// index are stored in the top bit of the first two words). Max error ≈ 3e-5 per component.
struct PackedQuat {
    u16 a = 0, b = 0, c = 0;

    static PackedQuat pack(const glm::quat& q);
    glm::quat unpack() const;
};

struct CompactClipSettings {
    bool quantizeRotations = true;
    // CubicSpline tracks are resampled to linear keys at this rate.
    f32 resampleRate = 30.0f;
};

// Runtime representation: all keys of all joints live in a few flat arrays (SoA), per-joint ranges are
// offsets. Only Step/Linear interpolation (cubic tracks are resampled on build).
class CompactClip {
public:
    static CompactClip build(const AnimationClip& clip, const CompactClipSettings& settings = {});

    void sample(const Skeleton& skeleton, f32 time, Pose& out, SamplingCursor* cursor = nullptr) const;
    AnimationClip toClip() const;

    std::string name;
    f32 duration = 0.0f;
    RotationBlend rotationBlend = RotationBlend::Slerp;
    bool additive = false;
    bool quantized = true;
    u32 jointCount = 0;

    // Per joint: [offset, offset + count) into the arrays below; index 3*j+{0,1,2} for T/R/S.
    std::vector<u32> rangeOffset;
    std::vector<u32> rangeCount;
    std::vector<u8> rangeStep; // 1 = step interpolation

    std::vector<f32> translationTimes;
    std::vector<glm::vec3> translations;
    std::vector<f32> rotationTimes;
    std::vector<glm::quat> rotations;          // when !quantized
    std::vector<PackedQuat> packedRotations;   // when quantized
    std::vector<f32> scaleTimes;
    std::vector<glm::vec3> scales;

    std::vector<AnimEvent> events;
    RootMotionTrack rootMotion;

    usize memoryBytes() const;
};

} // namespace ox::anim
