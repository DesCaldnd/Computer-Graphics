#pragma once

#include <oxwald/animation/pose.hpp>

#include <string>
#include <vector>

namespace ox::anim {

enum class Interpolation : u8 {
    Step,
    Linear,     // vec3 lerp / quaternion slerp or nlerp (see AnimationClip::rotationBlend)
    CubicSpline // glTF cubic Hermite: inTangents/outTangents are per-key, scaled by key delta time
};

enum class RotationBlend : u8 { Slerp, Nlerp };

// Keyframe track in SoA layout: times and values are separate arrays.
template <class T>
struct Track {
    std::vector<f32> times;     // strictly increasing, seconds
    std::vector<T> values;
    std::vector<T> inTangents;  // CubicSpline only (same size as values)
    std::vector<T> outTangents; // CubicSpline only
    Interpolation interpolation = Interpolation::Linear;

    bool empty() const { return times.empty(); }
    usize size() const { return times.size(); }
};

struct JointTrack {
    Track<glm::vec3> translation;
    Track<glm::quat> rotation;
    Track<glm::vec3> scale;
};

// Named notify fired when playback crosses `time`.
struct AnimEvent {
    f32 time = 0.0f;
    std::string name;
    f32 payload = 0.0f; // optional user value (e.g. footstep strength)
};

// Root motion extracted from the root joint: per-key root transform in model space, restricted to the
// enabled channels. Deltas between times are expressed in the character space of the start time.
struct RootMotionTrack {
    std::vector<f32> times;
    std::vector<glm::vec3> positions;
    std::vector<glm::quat> rotations; // yaw only

    bool empty() const { return times.empty(); }
    Transform sample(f32 time) const;
    // Delta from t0 to t1 within one cycle (t0 <= t1).
    Transform delta(f32 t0, f32 t1) const;
    // Delta over [t0, t1] spanning `wraps` loop boundaries forward (t1 measured in the last cycle).
    Transform deltaLooped(f32 t0, f32 t1, i32 wraps, f32 duration) const;
};

// Remembers the last key index per track so sequential sampling is O(1) instead of a binary search.
struct SamplingCursor {
    std::vector<u32> keys; // 3 per joint (T, R, S)
    const void* owner = nullptr;
    f32 lastTime = -1.0f;
    void reset() { owner = nullptr; lastTime = -1.0f; }
};

struct RootMotionSettings {
    i32 rootJoint = 0;
    bool translationXZ = true;
    bool translationY = false;
    bool yaw = true;
};

class AnimationClip {
public:
    std::string name;
    f32 duration = 0.0f;
    RotationBlend rotationBlend = RotationBlend::Slerp;
    std::vector<JointTrack> tracks; // indexed by skeleton joint; empty tracks fall back to bind / identity
    std::vector<AnimEvent> events;  // sorted by time
    RootMotionTrack rootMotion;
    // Additive clips store deltas against a reference pose; missing tracks mean "no change".
    bool additive = false;

    // Samples the clip at `time` (clamped to [0, duration]) into `out` (resized to the skeleton).
    void sample(const Skeleton& skeleton, f32 time, Pose& out, SamplingCursor* cursor = nullptr) const;

    void sortEvents();
    void addEvent(f32 time, std::string eventName, f32 payload = 0.0f);
    // Events with time in (t0, t1]; if t1 < t0 the range wraps over the loop point.
    void collectEvents(f32 t0, f32 t1, std::vector<const AnimEvent*>& out) const;

    // Recomputes `duration` from the last keyframe.
    void computeDuration();
    // Makes consecutive rotation keys lie in the same hemisphere (avoids long-way-round interpolation).
    void fixQuaternionHemispheres();
};

// Removes the root's horizontal (and optionally vertical / yaw) motion from `clip` and stores it in
// `clip.rootMotion`. The root keeps its initial value for the extracted channels.
void extractRootMotion(AnimationClip& clip, const Skeleton& skeleton, const RootMotionSettings& settings);

// Converts `clip` to an additive clip relative to `reference` (usually the clip's first frame or the bind pose).
AnimationClip makeAdditiveClip(const AnimationClip& clip, const Skeleton& skeleton, const Pose& reference);

// Samples a track at `time` (exposed for tests and tools). `cursor` may be null.
glm::vec3 sampleTrack(const Track<glm::vec3>& track, f32 time, u32* cursor = nullptr);
glm::quat sampleTrack(const Track<glm::quat>& track, f32 time, RotationBlend blend, u32* cursor = nullptr);

} // namespace ox::anim
