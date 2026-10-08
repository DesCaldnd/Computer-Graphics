#include <oxwald/animation/compact_clip.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace ox::anim {

namespace {

constexpr f32 kQuatRange = 0.70710678118f; // smallest-three components lie in [-1/√2, 1/√2]
constexpr f32 kQuatScale = 32767.0f;

u16 quantize15(f32 v) {
    const f32 n = std::clamp(v / kQuatRange * 0.5f + 0.5f, 0.0f, 1.0f);
    return static_cast<u16>(std::lround(n * kQuatScale));
}

f32 dequantize15(u16 v) {
    return (static_cast<f32>(v & 0x7FFF) / kQuatScale * 2.0f - 1.0f) * kQuatRange;
}

u32 findKey(const f32* times, u32 n, f32 t, u32* cursor) {
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
    const f32* it = std::upper_bound(times, times + n, t);
    u32 k = it == times ? 0u : static_cast<u32>(it - times) - 1u;
    k = std::min(k, n - 2);
    if (cursor) {
        *cursor = k;
    }
    return k;
}

// Returns (k, s) for the segment containing t; s in [0,1].
std::pair<u32, f32> locate(const f32* times, u32 n, f32 t, u32* cursor) {
    if (n == 1 || t <= times[0]) {
        return {0u, 0.0f};
    }
    if (t >= times[n - 1]) {
        return {n - 2, 1.0f};
    }
    const u32 k = findKey(times, n, t, cursor);
    const f32 dt = times[k + 1] - times[k];
    return {k, dt > 0.0f ? (t - times[k]) / dt : 0.0f};
}

template <class T, class Sampler>
void appendResampled(const Track<T>& track, f32 duration, f32 rate, std::vector<f32>& times, std::vector<T>& values,
                     Sampler&& sampler) {
    if (track.interpolation != Interpolation::CubicSpline) {
        times.insert(times.end(), track.times.begin(), track.times.end());
        values.insert(values.end(), track.values.begin(), track.values.end());
        return;
    }
    const f32 start = track.times.front();
    const f32 end = track.times.back();
    const u32 steps = std::max(1u, static_cast<u32>(std::ceil((end - start) * rate)));
    for (u32 i = 0; i <= steps; ++i) {
        const f32 t = start + (end - start) * static_cast<f32>(i) / static_cast<f32>(steps);
        times.push_back(t);
        values.push_back(sampler(t));
    }
    (void)duration;
}

} // namespace

PackedQuat PackedQuat::pack(const glm::quat& qIn) {
    const glm::quat q = glm::normalize(qIn);
    f32 c[4] = {q.x, q.y, q.z, q.w};
    u32 largest = 0;
    for (u32 i = 1; i < 4; ++i) {
        if (std::abs(c[i]) > std::abs(c[largest])) {
            largest = i;
        }
    }
    const f32 sign = c[largest] < 0.0f ? -1.0f : 1.0f;
    u16 out[3];
    u32 o = 0;
    for (u32 i = 0; i < 4; ++i) {
        if (i != largest) {
            out[o++] = quantize15(c[i] * sign);
        }
    }
    PackedQuat p;
    p.a = static_cast<u16>(out[0] | ((largest & 1u) << 15));
    p.b = static_cast<u16>(out[1] | (((largest >> 1) & 1u) << 15));
    p.c = out[2];
    return p;
}

glm::quat PackedQuat::unpack() const {
    const u32 largest = static_cast<u32>((a >> 15) | ((b >> 15) << 1));
    const f32 v[3] = {dequantize15(a), dequantize15(b), dequantize15(c)};
    f32 c4[4];
    u32 o = 0;
    f32 sum = 0.0f;
    for (u32 i = 0; i < 4; ++i) {
        if (i != largest) {
            c4[i] = v[o++];
            sum += c4[i] * c4[i];
        }
    }
    c4[largest] = std::sqrt(std::max(0.0f, 1.0f - sum));
    return glm::normalize(glm::quat(c4[3], c4[0], c4[1], c4[2]));
}

CompactClip CompactClip::build(const AnimationClip& clip, const CompactClipSettings& settings) {
    CompactClip out;
    out.name = clip.name;
    out.duration = clip.duration;
    out.rotationBlend = clip.rotationBlend;
    out.additive = clip.additive;
    out.quantized = settings.quantizeRotations;
    out.jointCount = static_cast<u32>(clip.tracks.size());
    out.events = clip.events;
    out.rootMotion = clip.rootMotion;
    out.rangeOffset.resize(out.jointCount * 3);
    out.rangeCount.resize(out.jointCount * 3);
    out.rangeStep.resize(out.jointCount * 3);

    std::vector<glm::quat> rotations;
    for (u32 j = 0; j < out.jointCount; ++j) {
        const JointTrack& jt = clip.tracks[j];
        const u32 base = j * 3;

        out.rangeOffset[base] = static_cast<u32>(out.translationTimes.size());
        if (!jt.translation.empty()) {
            appendResampled(jt.translation, clip.duration, settings.resampleRate, out.translationTimes,
                            out.translations, [&](f32 t) { return sampleTrack(jt.translation, t); });
        }
        out.rangeCount[base] = static_cast<u32>(out.translationTimes.size()) - out.rangeOffset[base];
        out.rangeStep[base] = jt.translation.interpolation == Interpolation::Step;

        out.rangeOffset[base + 1] = static_cast<u32>(out.rotationTimes.size());
        if (!jt.rotation.empty()) {
            appendResampled(jt.rotation, clip.duration, settings.resampleRate, out.rotationTimes, rotations,
                            [&](f32 t) { return sampleTrack(jt.rotation, t, clip.rotationBlend); });
        }
        out.rangeCount[base + 1] = static_cast<u32>(out.rotationTimes.size()) - out.rangeOffset[base + 1];
        out.rangeStep[base + 1] = jt.rotation.interpolation == Interpolation::Step;

        out.rangeOffset[base + 2] = static_cast<u32>(out.scaleTimes.size());
        if (!jt.scale.empty()) {
            appendResampled(jt.scale, clip.duration, settings.resampleRate, out.scaleTimes, out.scales,
                            [&](f32 t) { return sampleTrack(jt.scale, t); });
        }
        out.rangeCount[base + 2] = static_cast<u32>(out.scaleTimes.size()) - out.rangeOffset[base + 2];
        out.rangeStep[base + 2] = jt.scale.interpolation == Interpolation::Step;
    }
    // Hemisphere-fix after resampling, then (optionally) quantise.
    for (u32 j = 0; j < out.jointCount; ++j) {
        const u32 off = out.rangeOffset[j * 3 + 1];
        const u32 cnt = out.rangeCount[j * 3 + 1];
        for (u32 k = 1; k < cnt; ++k) {
            if (glm::dot(rotations[off + k - 1], rotations[off + k]) < 0.0f) {
                rotations[off + k] = -rotations[off + k];
            }
        }
    }
    if (out.quantized) {
        out.packedRotations.reserve(rotations.size());
        for (const auto& q : rotations) {
            out.packedRotations.push_back(PackedQuat::pack(q));
        }
    } else {
        out.rotations = std::move(rotations);
    }
    return out;
}

void CompactClip::sample(const Skeleton& skeleton, f32 time, Pose& out, SamplingCursor* cursor) const {
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
        if (j < jointCount) {
            const usize base = j * 3;
            u32* c = cursor ? &cursor->keys[base] : nullptr;
            if (const u32 n = rangeCount[base]; n > 0) {
                const u32 off = rangeOffset[base];
                auto [k, s] = locate(translationTimes.data() + off, n, time, c);
                t.translation = (n == 1 || rangeStep[base]) && s < 1.0f
                                    ? translations[off + k]
                                    : glm::mix(translations[off + k], translations[off + std::min(k + 1, n - 1)], s);
            }
            if (const u32 n = rangeCount[base + 1]; n > 0) {
                const u32 off = rangeOffset[base + 1];
                auto [k, s] = locate(rotationTimes.data() + off, n, time, c ? c + 1 : nullptr);
                auto rot = [&](u32 i) { return quantized ? packedRotations[off + i].unpack() : rotations[off + i]; };
                if ((n == 1 || rangeStep[base + 1]) && s < 1.0f) {
                    t.rotation = rot(k);
                } else {
                    const glm::quat a = rot(k);
                    const glm::quat b = rot(std::min(k + 1, n - 1));
                    t.rotation = rotationBlend == RotationBlend::Slerp ? slerpShortest(a, b, s) : nlerpShortest(a, b, s);
                }
            }
            if (const u32 n = rangeCount[base + 2]; n > 0) {
                const u32 off = rangeOffset[base + 2];
                auto [k, s] = locate(scaleTimes.data() + off, n, time, c ? c + 2 : nullptr);
                t.scale = (n == 1 || rangeStep[base + 2]) && s < 1.0f
                              ? scales[off + k]
                              : glm::mix(scales[off + k], scales[off + std::min(k + 1, n - 1)], s);
            }
        }
        out.local[j] = t;
    }
    if (cursor) {
        cursor->lastTime = time;
    }
}

AnimationClip CompactClip::toClip() const {
    AnimationClip clip;
    clip.name = name;
    clip.duration = duration;
    clip.rotationBlend = rotationBlend;
    clip.additive = additive;
    clip.events = events;
    clip.rootMotion = rootMotion;
    clip.tracks.resize(jointCount);
    for (u32 j = 0; j < jointCount; ++j) {
        JointTrack& jt = clip.tracks[j];
        const u32 base = j * 3;
        auto interp = [&](u32 r) { return rangeStep[r] ? Interpolation::Step : Interpolation::Linear; };
        for (u32 k = 0; k < rangeCount[base]; ++k) {
            jt.translation.times.push_back(translationTimes[rangeOffset[base] + k]);
            jt.translation.values.push_back(translations[rangeOffset[base] + k]);
        }
        jt.translation.interpolation = interp(base);
        for (u32 k = 0; k < rangeCount[base + 1]; ++k) {
            const u32 i = rangeOffset[base + 1] + k;
            jt.rotation.times.push_back(rotationTimes[i]);
            jt.rotation.values.push_back(quantized ? packedRotations[i].unpack() : rotations[i]);
        }
        jt.rotation.interpolation = interp(base + 1);
        for (u32 k = 0; k < rangeCount[base + 2]; ++k) {
            jt.scale.times.push_back(scaleTimes[rangeOffset[base + 2] + k]);
            jt.scale.values.push_back(scales[rangeOffset[base + 2] + k]);
        }
        jt.scale.interpolation = interp(base + 2);
    }
    return clip;
}

usize CompactClip::memoryBytes() const {
    return rangeOffset.size() * 4 + rangeCount.size() * 4 + rangeStep.size() + translationTimes.size() * 4 +
           translations.size() * sizeof(glm::vec3) + rotationTimes.size() * 4 + rotations.size() * sizeof(glm::quat) +
           packedRotations.size() * sizeof(PackedQuat) + scaleTimes.size() * 4 + scales.size() * sizeof(glm::vec3);
}

} // namespace ox::anim
