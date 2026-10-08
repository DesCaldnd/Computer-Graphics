// Глава 10: скелет, клип, сэмплирование, события, compact clip, сериализация (docs/guide/10-animation.md).
#include "guide_rig.hpp"

#include <gtest/gtest.h>

using namespace ox;
using namespace ox::anim;

TEST(GuideAnimationClips, BuildAndSampleClip) {
    const Skeleton skel = guide::makeArm();
    EXPECT_EQ(skel.jointCount(), 3u);
    EXPECT_EQ(skel.parent(skel.findJoint("Wrist")), skel.findJoint("Elbow"));

    // Клип 1 с: локоть сгибается 0 -> 90°, плечо уезжает на 2 м по +Z.
    AnimationClip walk;
    walk.name = "bend";
    walk.tracks.resize(skel.jointCount());
    auto& elbowRot = walk.tracks[1].rotation;
    elbowRot.times = {0.0f, 1.0f};
    elbowRot.values = {glm::quat(1, 0, 0, 0), glm::angleAxis(glm::half_pi<f32>(), glm::vec3(0, 0, 1))};
    auto& rootPos = walk.tracks[0].translation;
    rootPos.times = {0.0f, 1.0f};
    rootPos.values = {{0, 0, 0}, {0, 0, 2}};
    walk.computeDuration(); // = время последнего ключа
    walk.addEvent(0.5f, "footstep", 0.8f);

    Pose pose;
    walk.sample(skel, 0.5f, pose); // pose.local: по Transform на сустав
    EXPECT_NEAR(guide::elbowAngle(pose), glm::quarter_pi<f32>(), 1e-4f);
    EXPECT_NEAR(pose.local[0].translation.z, 1.0f, 1e-5f);

    // Последовательное воспроизведение: курсор кеширует индексы ключей (O(1)).
    SamplingCursor cursor;
    for (f32 t = 0.0f; t <= 1.0f; t += 1.0f / 60.0f) walk.sample(skel, t, pose, &cursor);

    // События в окне (t0, t1].
    std::vector<const AnimEvent*> fired;
    walk.collectEvents(0.4f, 0.6f, fired);
    ASSERT_EQ(fired.size(), 1u);
    EXPECT_EQ(fired[0]->name, "footstep");

    // Модельное пространство и матрицы.
    std::vector<Transform> model;
    localToModel(skel, pose, model);
    EXPECT_NEAR(model[2].translation.y, 1.0f, 1e-3f); // кисть ушла вбок: (-1, 1, z)
    EXPECT_NEAR(model[2].translation.x, -1.0f, 1e-3f);
}

TEST(GuideAnimationClips, CompactClipAndSerialization) {
    const Skeleton skel = guide::makeArm();
    AnimationClip clip = *guide::makePoseClip("idle", 0.0f, 2.0f);
    clip.tracks[1].rotation.times = {0.0f, 1.0f, 2.0f};
    clip.tracks[1].rotation.values = {glm::quat(1, 0, 0, 0), glm::angleAxis(1.0f, glm::vec3(0, 0, 1)),
                                      glm::quat(1, 0, 0, 0)};

    // Компактный рантайм-формат: плоские массивы, кватернионы 48 бит.
    const CompactClip compact = CompactClip::build(clip, {.quantizeRotations = true, .resampleRate = 30.0f});
    EXPECT_LT(compact.memoryBytes(), CompactClip::build(clip, {.quantizeRotations = false}).memoryBytes());
    Pose a, b;
    clip.sample(skel, 0.7f, a);
    compact.sample(skel, 0.7f, b);
    EXPECT_NEAR(guide::elbowAngle(a), guide::elbowAngle(b), 1e-3f);

    // Бинарная сериализация (little-endian, magic + версия, проверка границ).
    ByteWriter w;
    serialize(w, skel);
    serialize(w, clip);
    serialize(w, compact);
    const std::vector<u8> bytes = w.take();

    ByteReader r(bytes);
    Skeleton skel2;
    AnimationClip clip2;
    CompactClip compact2;
    ASSERT_TRUE(deserialize(r, skel2));
    ASSERT_TRUE(deserialize(r, clip2));
    ASSERT_TRUE(deserialize(r, compact2));
    EXPECT_EQ(skel2.jointName(2), "Wrist");
    EXPECT_FLOAT_EQ(clip2.duration, 2.0f);

    // Обрезанные данные -> false, без чтения за границей.
    ByteReader truncated(std::span<const u8>(bytes.data(), bytes.size() / 2));
    Skeleton s3;
    AnimationClip c3;
    EXPECT_FALSE(deserialize(truncated, s3) && deserialize(truncated, c3));
}
