// Глава 10: CPU-скиннинг (LBS / dual quaternion), палитра, debug draw (docs/guide/10-animation.md).
#include "guide_rig.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace ox;
using namespace ox::anim;

TEST(GuideAnimationSkinning, SkinBarOnCpu) {
    const Skeleton skel = guide::makeArm();

    // Брусок из трёх колец по 4 вершины; среднее кольцо делят плечо и локоть 50/50.
    SkinnedMeshData bar;
    std::vector<std::vector<std::pair<u16, f32>>> influences;
    for (int ring = 0; ring < 3; ++ring) {
        for (int k = 0; k < 4; ++k) {
            const f32 a = glm::half_pi<f32>() * static_cast<f32>(k);
            bar.positions.push_back({0.2f * std::cos(a), static_cast<f32>(ring), 0.2f * std::sin(a)});
            bar.normals.push_back({std::cos(a), 0.0f, std::sin(a)});
            if (ring == 0) influences.push_back({{0, 1.0f}});
            else if (ring == 1) influences.push_back({{0, 0.5f}, {1, 0.5f}});
            else influences.push_back({{1, 1.0f}});
        }
    }
    bar.setInfluences(influences, 4); // <= 4 влияний, сортировка и нормализация

    Pose pose;
    pose.setBind(skel);
    pose.local[1].rotation = glm::angleAxis(glm::half_pi<f32>(), glm::vec3(0, 0, 1)); // согнуть локоть

    std::vector<glm::mat4> model, palette;
    localToModel(skel, pose, model);
    computeSkinningMatrices(skel, model, palette); // palette[j] = model[j] * inverseBind[j]

    std::vector<glm::vec3> lbs, dqs, normals;
    skinMesh(bar, palette, SkinningMethod::Linear, lbs, &normals);
    skinMesh(bar, palette, SkinningMethod::DualQuaternion, dqs);

    // Верхнее кольцо (целиком на локте): центр (0,2,0) -> (-1,1,0).
    glm::vec3 top(0.0f);
    for (int k = 8; k < 12; ++k) top += lbs[static_cast<usize>(k)] * 0.25f;
    EXPECT_NEAR(top.x, -1.0f, 1e-4f);
    EXPECT_NEAR(top.y, 1.0f, 1e-4f);

    // DQS сохраняет объём на сгибе лучше LBS («candy wrapper»).
    auto ringRadius = [](const std::vector<glm::vec3>& v) {
        glm::vec3 c(0.0f);
        for (int k = 4; k < 8; ++k) c += v[static_cast<usize>(k)] * 0.25f;
        f32 r = 0.0f;
        for (int k = 4; k < 8; ++k) r += glm::length(v[static_cast<usize>(k)] - c) * 0.25f;
        return r;
    };
    EXPECT_LT(ringRadius(lbs), ringRadius(dqs));
    EXPECT_NEAR(ringRadius(dqs), 0.2f, 2e-3f);

    // Палитра для GPU-варианта на dual quaternion: 2 x vec4 на сустав.
    std::vector<DualQuat> dqPalette;
    computeDualQuatPalette(palette, dqPalette);
    EXPECT_EQ(dqPalette.size(), skel.jointCount());
}

TEST(GuideAnimationSkinning, DebugDrawSkeleton) {
    const Skeleton skel = guide::makeArm();
    Pose pose;
    pose.setBind(skel);
    std::vector<Transform> model;
    localToModel(skel, pose, model);

    Transform ownerWorld;
    ownerWorld.translation = {10, 0, 0};
    int bones = 0;
    glm::vec3 lastTo{0.0f};
    debugDrawSkeleton(
        skel, model, ownerWorld,
        [&](glm::vec3, glm::vec3 to, glm::vec4) {
            ++bones;
            lastTo = to;
        },
        {1, 0.8f, 0.2f, 1}, /*axisLength*/ 0.0f); // 0 — без осей суставов
    EXPECT_EQ(bones, 2); // родитель -> ребёнок
    EXPECT_NEAR(lastTo.x, 10.0f, 1e-5f);
}
