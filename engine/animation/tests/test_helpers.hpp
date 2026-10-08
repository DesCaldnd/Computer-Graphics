#pragma once

#include <oxwald/animation/animation.hpp>

#include <gtest/gtest.h>

#include <glm/gtc/constants.hpp>

#include <cmath>
#include <memory>

namespace ox::anim::test {

inline constexpr f32 kTol = 1e-4f;

// Root (origin) → Mid (+1 Y) → Tip (+1 Y): a straight vertical 2-bone chain.
inline Skeleton makeChainSkeleton() {
    Skeleton s;
    const i32 root = s.addJoint("Root", kNoJoint, Transform{});
    const i32 mid = s.addJoint("Mid", root, Transform{{0, 1, 0}, {1, 0, 0, 0}, {1, 1, 1}});
    s.addJoint("Tip", mid, Transform{{0, 1, 0}, {1, 0, 0, 0}, {1, 1, 1}});
    s.finalize();
    return s;
}

// Mid rotates 0 → 90° around Z over 1 s; Root translates along +Z by `distance` (root motion source).
inline AnimationClip makeBendClip(f32 distance = 0.0f, f32 duration = 1.0f) {
    AnimationClip c;
    c.name = "bend";
    c.tracks.resize(3);
    auto& mid = c.tracks[1].rotation;
    mid.times = {0.0f, duration};
    mid.values = {glm::quat(1, 0, 0, 0), glm::angleAxis(glm::half_pi<f32>(), glm::vec3(0, 0, 1))};
    if (distance != 0.0f) {
        auto& root = c.tracks[0].translation;
        root.times = {0.0f, duration * 0.5f, duration};
        root.values = {{0, 0, 0}, {0, 0, distance * 0.5f}, {0, 0, distance}};
    }
    c.duration = duration;
    return c;
}

// Constant pose clip: Mid rotated by `angle` around Z.
inline std::shared_ptr<AnimationClip> makeStaticClip(const char* name, f32 angle, f32 duration = 1.0f) {
    auto c = std::make_shared<AnimationClip>();
    c->name = name;
    c->tracks.resize(3);
    c->tracks[1].rotation.times = {0.0f, duration};
    const glm::quat q = glm::angleAxis(angle, glm::vec3(0, 0, 1));
    c->tracks[1].rotation.values = {q, q};
    c->duration = duration;
    return c;
}

inline f32 midAngle(const Pose& p) {
    return glm::angle(p.local[1].rotation);
}

inline void expectVec(const glm::vec3& a, const glm::vec3& b, f32 tol = kTol) {
    EXPECT_NEAR(a.x, b.x, tol);
    EXPECT_NEAR(a.y, b.y, tol);
    EXPECT_NEAR(a.z, b.z, tol);
}

inline void expectQuat(const glm::quat& a, const glm::quat& b, f32 tol = kTol) {
    EXPECT_NEAR(std::abs(glm::dot(a, b)), 1.0f, tol) << "quats differ";
}

} // namespace ox::anim::test
