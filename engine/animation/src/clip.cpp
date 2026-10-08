#include <oxwald/animation/clip.hpp>

#include <oxwald/core/assert.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace ox::anim {

namespace {

// Index k with times[k] <= t < times[k+1], clamped to [0, n-2]. Uses the cursor for O(1) forward playback.
u32 findKey(const std::vector<f32>& times, f32 t, u32* cursor) {
    const u32 n = static_cast<u32>(times.size());
    if (n < 2) {
        return 0;
    }
    if (cursor) {
        u32 k = std::min(*cursor, n - 2);
        if (times[k] <= t) {
            for (u32 step = 0; step < 4; ++step) {
                if (k + 1 >= n - 1 || times[k + 1] > t) {
                    *cursor = k;
                    return k;
                }
                ++k;
            }
        }
    }
    const auto it = std::upper_bound(times.begin(), times.end(), t);
    u32 k = it == times.begin() ? 0u : static_cast<u32>(it - times.begin()) - 1u;
    k = std::min(k, n - 2);
    if (cursor) {
        *cursor = k;
    }
    return k;
}

template <class T>
T hermite(const T& p0, const T& m0, const T& p1, const T& m1, f32 s) {
    const f32 s2 = s * s;
    const f32 s3 = s2 * s;
    return p0 * (2 * s3 - 3 * s2 + 1) + m0 * (s3 - 2 * s2 + s) + p1 * (-2 * s3 + 3 * s2) + m1 * (s3 - s2);
}

glm::quat yawOf(const glm::quat& q) {
    // Swing-twist decomposition around +Y.
    glm::quat twist(q.w, 0.0f, q.y, 0.0f);
    const f32 len = glm::length(twist);
    return len < 1e-6f ? glm::quat(1, 0, 0, 0) : twist / len;
}

Transform compose(const Transform& a, const Transform& b) {
    return {a.translation + a.rotation * b.translation, glm::normalize(a.rotation * b.rotation), glm::vec3(1.0f)};
}

} // namespace

glm::vec3 sampleTrack(const Track<glm::vec3>& track, f32 time, u32* cursor) {
    const usize n = track.times.size();
    if (n == 0) {
        return glm::vec3(0.0f);
    }
    if (n == 1 || time <= track.times.front()) {
        return track.values.front();
    }
    if (time >= track.times.back()) {
        return track.values.back();
    }
    const u32 k = findKey(track.times, time, cursor);
    const f32 t0 = track.times[k];
    const f32 dt = track.times[k + 1] - t0;
    const f32 s = dt > 0.0f ? (time - t0) / dt : 0.0f;
    switch (track.interpolation) {
    case Interpolation::Step:
        return track.values[k];
    case Interpolation::CubicSpline:
        return hermite(track.values[k], track.outTangents[k] * dt, track.values[k + 1], track.inTangents[k + 1] * dt, s);
    case Interpolation::Linear:
    default:
        return glm::mix(track.values[k], track.values[k + 1], s);
    }
}

glm::quat sampleTrack(const Track<glm::quat>& track, f32 time, RotationBlend blend, u32* cursor) {
    const usize n = track.times.size();
    if (n == 0) {
        return glm::quat(1, 0, 0, 0);
    }
    if (n == 1 || time <= track.times.front()) {
        return track.values.front();
    }
    if (time >= track.times.back()) {
        return track.values.back();
    }
    const u32 k = findKey(track.times, time, cursor);
    const f32 t0 = track.times[k];
    const f32 dt = track.times[k + 1] - t0;
    const f32 s = dt > 0.0f ? (time - t0) / dt : 0.0f;
    switch (track.interpolation) {
    case Interpolation::Step:
        return track.values[k];
    case Interpolation::CubicSpline: {
        const glm::quat q =
            hermite(track.values[k], track.outTangents[k] * dt, track.values[k + 1], track.inTangents[k + 1] * dt, s);
        return glm::normalize(q);
    }
    case Interpolation::Linear:
    default:
        return blend == RotationBlend::Slerp ? slerpShortest(track.values[k], track.values[k + 1], s)
                                             : nlerpShortest(track.values[k], track.values[k + 1], s);
    }
}

void AnimationClip::sample(const Skeleton& skeleton, f32 time, Pose& out, SamplingCursor* cursor) const {
    const usize joints = skeleton.jointCount();
    out.resize(joints);
    time = std::clamp(time, 0.0f, duration);
    if (cursor && (cursor->owner != this || cursor->keys.size() != joints * 3)) {
        cursor->keys.assign(joints * 3, 0u);
        cursor->owner = this;
    }
    const auto& bind = skeleton.bindPose();
    for (usize j = 0; j < joints; ++j) {
        Transform t = additive ? Transform{} : bind[j];
        if (j < tracks.size()) {
            const JointTrack& jt = tracks[j];
            u32* c = cursor ? &cursor->keys[j * 3] : nullptr;
            if (!jt.translation.empty()) {
                t.translation = sampleTrack(jt.translation, time, c);
            }
            if (!jt.rotation.empty()) {
                t.rotation = sampleTrack(jt.rotation, time, rotationBlend, c ? c + 1 : nullptr);
            }
            if (!jt.scale.empty()) {
                t.scale = sampleTrack(jt.scale, time, c ? c + 2 : nullptr);
            }
        }
        out.local[j] = t;
    }
    if (cursor) {
        cursor->lastTime = time;
    }
}

void AnimationClip::sortEvents() {
    std::stable_sort(events.begin(), events.end(), [](const AnimEvent& a, const AnimEvent& b) { return a.time < b.time; });
}

void AnimationClip::addEvent(f32 time, std::string eventName, f32 payload) {
    events.push_back({time, std::move(eventName), payload});
    sortEvents();
}

void AnimationClip::collectEvents(f32 t0, f32 t1, std::vector<const AnimEvent*>& out) const {
    if (t1 >= t0) {
        for (const auto& e : events) {
            if (e.time > t0 && e.time <= t1) {
                out.push_back(&e);
            }
        }
    } else {
        for (const auto& e : events) {
            if (e.time > t0) {
                out.push_back(&e);
            }
        }
        for (const auto& e : events) {
            if (e.time <= t1) {
                out.push_back(&e);
            }
        }
    }
}

void AnimationClip::computeDuration() {
    f32 d = 0.0f;
    for (const auto& jt : tracks) {
        if (!jt.translation.empty()) d = std::max(d, jt.translation.times.back());
        if (!jt.rotation.empty()) d = std::max(d, jt.rotation.times.back());
        if (!jt.scale.empty()) d = std::max(d, jt.scale.times.back());
    }
    duration = d;
}

void AnimationClip::fixQuaternionHemispheres() {
    for (auto& jt : tracks) {
        auto& v = jt.rotation.values;
        const bool cubic = jt.rotation.interpolation == Interpolation::CubicSpline;
        for (usize k = 1; k < v.size(); ++k) {
            if (glm::dot(v[k - 1], v[k]) < 0.0f) {
                v[k] = -v[k];
                if (cubic) {
                    jt.rotation.inTangents[k] = -jt.rotation.inTangents[k];
                    jt.rotation.outTangents[k] = -jt.rotation.outTangents[k];
                }
            }
        }
    }
}

// ---- Root motion ----

Transform RootMotionTrack::sample(f32 time) const {
    if (times.empty()) {
        return {};
    }
    if (time <= times.front()) {
        return {positions.front(), rotations.front(), glm::vec3(1.0f)};
    }
    if (time >= times.back()) {
        return {positions.back(), rotations.back(), glm::vec3(1.0f)};
    }
    const u32 k = findKey(times, time, nullptr);
    const f32 dt = times[k + 1] - times[k];
    const f32 s = dt > 0.0f ? (time - times[k]) / dt : 0.0f;
    return {glm::mix(positions[k], positions[k + 1], s), nlerpShortest(rotations[k], rotations[k + 1], s),
            glm::vec3(1.0f)};
}

Transform RootMotionTrack::delta(f32 t0, f32 t1) const {
    const Transform a = sample(t0);
    const Transform b = sample(t1);
    const glm::quat inv = glm::conjugate(a.rotation);
    return {inv * (b.translation - a.translation), glm::normalize(inv * b.rotation), glm::vec3(1.0f)};
}

Transform RootMotionTrack::deltaLooped(f32 t0, f32 t1, i32 wraps, f32 duration) const {
    if (wraps == 0) {
        return delta(t0, t1);
    }
    if (wraps > 0) {
        Transform d = delta(t0, duration);
        const Transform cycle = delta(0.0f, duration);
        for (i32 i = 1; i < wraps; ++i) {
            d = compose(d, cycle);
        }
        return compose(d, delta(0.0f, t1));
    }
    Transform d = delta(t0, 0.0f);
    const Transform cycle = delta(duration, 0.0f);
    for (i32 i = 1; i < -wraps; ++i) {
        d = compose(d, cycle);
    }
    return compose(d, delta(duration, t1));
}

void extractRootMotion(AnimationClip& clip, const Skeleton& skeleton, const RootMotionSettings& settings) {
    const i32 root = settings.rootJoint;
    OX_ASSERT(root >= 0 && static_cast<usize>(root) < skeleton.jointCount(), "invalid root joint {}", root);
    if (static_cast<usize>(root) >= clip.tracks.size()) {
        return;
    }
    JointTrack& jt = clip.tracks[static_cast<usize>(root)];
    if (jt.translation.empty() && jt.rotation.empty()) {
        return;
    }
    std::vector<f32> times;
    times.insert(times.end(), jt.translation.times.begin(), jt.translation.times.end());
    times.insert(times.end(), jt.rotation.times.begin(), jt.rotation.times.end());
    const bool cubic = jt.translation.interpolation == Interpolation::CubicSpline ||
                       jt.rotation.interpolation == Interpolation::CubicSpline;
    if (cubic) {
        // Resampling to linear keys: densify so the curve shape survives.
        for (f32 t = 0.0f; t < clip.duration; t += 1.0f / 30.0f) {
            times.push_back(t);
        }
    }
    times.push_back(0.0f);
    times.push_back(clip.duration);
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end(), [](f32 a, f32 b) { return std::abs(a - b) < 1e-6f; }),
                times.end());

    const Transform& bind = skeleton.bindPose()[static_cast<usize>(root)];
    auto sampleT = [&](f32 t) { return jt.translation.empty() ? bind.translation : sampleTrack(jt.translation, t); };
    auto sampleR = [&](f32 t) {
        return jt.rotation.empty() ? bind.rotation : sampleTrack(jt.rotation, t, clip.rotationBlend);
    };

    const glm::vec3 t0 = sampleT(times.front());
    const glm::quat yaw0Inv = glm::conjugate(yawOf(sampleR(times.front())));

    RootMotionTrack motion;
    Track<glm::vec3> newT;
    Track<glm::quat> newR;
    for (f32 t : times) {
        const glm::vec3 tr = sampleT(t);
        const glm::quat rot = sampleR(t);
        glm::vec3 p = tr - t0;
        if (!settings.translationXZ) {
            p.x = 0.0f;
            p.z = 0.0f;
        }
        if (!settings.translationY) {
            p.y = 0.0f;
        }
        const glm::quat yaw = settings.yaw ? glm::normalize(yawOf(rot) * yaw0Inv) : glm::quat(1, 0, 0, 0);
        motion.times.push_back(t);
        motion.positions.push_back(p);
        motion.rotations.push_back(yaw);
        // root' = inv(M) * root so that M * root' reproduces the original motion.
        const glm::quat invYaw = glm::conjugate(yaw);
        newT.times.push_back(t);
        newT.values.push_back(invYaw * (tr - p));
        newR.times.push_back(t);
        newR.values.push_back(glm::normalize(invYaw * rot));
    }
    // The yaw-compensated translation keeps the root's pivot relative to the motion frame origin.
    // Shift so the first frame is unchanged: (tr - p) at t0 == t0 because p(t0) == 0, yaw(t0) == I.
    jt.translation = std::move(newT);
    jt.rotation = std::move(newR);
    clip.rootMotion = std::move(motion);
}

AnimationClip makeAdditiveClip(const AnimationClip& clip, const Skeleton& skeleton, const Pose& reference) {
    OX_ASSERT(reference.size() == skeleton.jointCount(), "reference pose size mismatch");
    AnimationClip out = clip;
    out.additive = true;
    out.name = clip.name + "_additive";
    for (usize j = 0; j < out.tracks.size() && j < reference.size(); ++j) {
        JointTrack& jt = out.tracks[j];
        const Transform& ref = reference.local[j];
        for (auto& v : jt.translation.values) v -= ref.translation;
        const glm::quat inv = glm::inverse(ref.rotation);
        for (auto& v : jt.rotation.values) v = glm::normalize(inv * v);
        for (auto& v : jt.rotation.inTangents) v = inv * v;
        for (auto& v : jt.rotation.outTangents) v = inv * v;
        for (auto& v : jt.scale.values) v /= ref.scale;
        for (auto& v : jt.scale.inTangents) v /= ref.scale;
        for (auto& v : jt.scale.outTangents) v /= ref.scale;
    }
    out.fixQuaternionHemispheres();
    return out;
}

} // namespace ox::anim
