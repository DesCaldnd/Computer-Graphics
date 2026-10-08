// Глава 10: блендинг поз, маски, аддитивы, blend spaces (docs/guide/10-animation.md).
#include "guide_rig.hpp"

#include <gtest/gtest.h>

using namespace ox;
using namespace ox::anim;

TEST(GuideAnimationBlending, PosesMasksAdditive) {
    const Skeleton skel = guide::makeArm();
    Pose straight, bent, out;
    guide::makePoseClip("straight", 0.0f)->sample(skel, 0.0f, straight);
    guide::makePoseClip("bent", glm::half_pi<f32>())->sample(skel, 0.0f, bent);

    // Линейный бленд двух поз (вращения — по кратчайшей дуге).
    blendPoses(straight, bent, 0.5f, out);
    EXPECT_NEAR(guide::elbowAngle(out), glm::quarter_pi<f32>(), 1e-4f);

    // Взвешенный бленд N поз: веса нормализуются.
    const Pose* poses[] = {&straight, &bent};
    const f32 weights[] = {3.0f, 1.0f};
    std::vector<f32> normalized = blendPosesWeighted(poses, weights, out);
    EXPECT_NEAR(normalized[0], 0.75f, 1e-6f);

    // Маска по ветке: только Wrist и его потомки.
    JointMask handOnly = JointMask::fromBranch(skel, skel.findJoint("Wrist"));
    blendPoses(straight, bent, 1.0f, out, &handOnly);
    EXPECT_NEAR(guide::elbowAngle(out), 0.0f, 1e-5f); // локоть не тронут

    // Аддитив: «+30° к локтю» поверх любой базовой позы.
    Pose bind;
    bind.setBind(skel);
    const AnimationClip lean = makeAdditiveClip(*guide::makePoseClip("lean", glm::radians(30.0f)), skel, bind);
    Pose delta;
    lean.sample(skel, 0.0f, delta);
    Pose result = bent;
    applyAdditive(result, delta, 1.0f);
    EXPECT_NEAR(guide::elbowAngle(result), glm::radians(120.0f), 1e-3f);
}

TEST(GuideAnimationBlending, BlendSpaces) {
    // 1D: скорость -> idle / walk / run.
    BlendSpace1D loco;
    loco.addSample(0.0f, guide::makePoseClip("idle", 0.0f));
    loco.addSample(1.5f, guide::makePoseClip("walk", 0.5f));
    loco.addSample(4.0f, guide::makePoseClip("run", 1.0f));
    std::vector<f32> w;
    loco.computeWeights(2.75f, w); // ровно между walk и run
    EXPECT_NEAR(w[1], 0.5f, 1e-6f);
    EXPECT_NEAR(w[2], 0.5f, 1e-6f);
    loco.computeWeights(10.0f, w); // за пределами — клэмп
    EXPECT_FLOAT_EQ(w[2], 1.0f);

    // 2D: (strafe, forward). Для локомоции — FreeformDirectional.
    BlendSpace2D strafe(BlendSpace2D::Mode::FreeformDirectional);
    strafe.addSample({0, 0}, guide::makePoseClip("idle", 0.0f));
    strafe.addSample({0, 2}, guide::makePoseClip("fwd", 0.2f));
    strafe.addSample({0, -2}, guide::makePoseClip("back", 0.4f));
    strafe.addSample({2, 0}, guide::makePoseClip("right", 0.6f));
    strafe.addSample({-2, 0}, guide::makePoseClip("left", 0.8f));
    strafe.computeWeights({0.0f, 2.0f}, w);
    EXPECT_NEAR(w[1], 1.0f, 1e-4f); // точно в сэмпле -> его вес 1
    strafe.computeWeights({1.0f, 1.0f}, w);
    f32 sum = 0.0f;
    for (f32 x : w) sum += x;
    EXPECT_NEAR(sum, 1.0f, 1e-5f); // веса всегда в сумме 1
}
