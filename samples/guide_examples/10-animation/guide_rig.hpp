// Глава 10: процедурный «скелет-рука» и клипы для примеров (без файлов ассетов).
#pragma once

#include <oxwald/animation/animation.hpp>

#include <glm/gtc/constants.hpp>

#include <memory>

namespace guide {

using namespace ox;
using namespace ox::anim;

// Shoulder (начало координат) -> Elbow (+1 по Y) -> Wrist (+1 по Y).
inline Skeleton makeArm() {
    Skeleton s;
    const i32 shoulder = s.addJoint("Shoulder", kNoJoint, Transform{});
    const i32 elbow = s.addJoint("Elbow", shoulder, Transform{{0, 1, 0}, {1, 0, 0, 0}, {1, 1, 1}});
    s.addJoint("Wrist", elbow, Transform{{0, 1, 0}, {1, 0, 0, 0}, {1, 1, 1}});
    s.finalize(); // inverse bind-матрицы из bind-позы
    return s;
}

// Клип-«поза»: локоть повёрнут на angle вокруг Z всё время клипа.
inline std::shared_ptr<AnimationClip> makePoseClip(const char* name, f32 angle, f32 duration = 1.0f) {
    auto c = std::make_shared<AnimationClip>();
    c->name = name;
    c->tracks.resize(3); // по треку на сустав; пустые треки -> bind-поза
    const glm::quat q = glm::angleAxis(angle, glm::vec3(0, 0, 1));
    c->tracks[1].rotation.times = {0.0f, duration};
    c->tracks[1].rotation.values = {q, q};
    c->duration = duration;
    return c;
}

inline f32 elbowAngle(const Pose& p) { return glm::angle(p.local[1].rotation); }

} // namespace guide
