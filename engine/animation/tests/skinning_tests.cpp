#include "test_helpers.hpp"

using namespace ox;
using namespace ox::anim;
using namespace ox::anim::test;

namespace {

void expectIdentity(const glm::mat4& m, f32 tol = 1e-5f) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) EXPECT_NEAR(m[c][r], c == r ? 1.0f : 0.0f, tol);
}

// Vertical bar from y=0 to y=2 with 3 rings; ring y=1 is split 50/50 between Root and Mid.
SkinnedMeshData makeBar() {
    SkinnedMeshData m;
    std::vector<std::vector<std::pair<u16, f32>>> inf;
    for (int ring = 0; ring < 3; ++ring) {
        for (int k = 0; k < 4; ++k) {
            const f32 a = glm::half_pi<f32>() * static_cast<f32>(k);
            m.positions.push_back({0.2f * std::cos(a), static_cast<f32>(ring), 0.2f * std::sin(a)});
            m.normals.push_back({std::cos(a), 0.0f, std::sin(a)});
            if (ring == 0) inf.push_back({{0, 1.0f}});
            else if (ring == 1) inf.push_back({{0, 0.5f}, {1, 0.5f}});
            else inf.push_back({{1, 1.0f}});
        }
    }
    m.setInfluences(inf, 4);
    return m;
}

} // namespace

TEST(AnimSkinning, BindPosePaletteIsIdentity) {
    const Skeleton s = makeChainSkeleton();
    Pose bind;
    bind.setBind(s);
    std::vector<glm::mat4> palette;
    computeSkinningMatrices(s, bind, palette);
    ASSERT_EQ(palette.size(), 3u);
    for (const auto& m : palette) expectIdentity(m);

    const SkinnedMeshData bar = makeBar();
    std::vector<glm::vec3> pos, nrm;
    for (auto method : {SkinningMethod::Linear, SkinningMethod::DualQuaternion}) {
        skinMesh(bar, palette, method, pos, &nrm);
        for (usize v = 0; v < pos.size(); ++v) {
            expectVec(pos[v], bar.positions[v], 1e-5f);
            expectVec(nrm[v], bar.normals[v], 1e-5f);
        }
    }
}

TEST(AnimSkinning, RigidBoneMovesVertices) {
    const Skeleton s = makeChainSkeleton();
    Pose pose;
    pose.setBind(s);
    pose.local[1].rotation = glm::angleAxis(glm::half_pi<f32>(), glm::vec3(0, 0, 1)); // bend Mid → -X
    std::vector<glm::mat4> palette;
    computeSkinningMatrices(s, pose, palette);
    const SkinnedMeshData bar = makeBar();
    std::vector<glm::vec3> lbs, dqs;
    skinMesh(bar, palette, SkinningMethod::Linear, lbs);
    skinMesh(bar, palette, SkinningMethod::DualQuaternion, dqs);
    // Top ring (fully Mid): centre (0,2,0) → (-1,1,0).
    glm::vec3 centre(0.0f);
    for (int k = 8; k < 12; ++k) centre += lbs[static_cast<usize>(k)] * 0.25f;
    expectVec(centre, {-1.0f, 1.0f, 0.0f}, 1e-5f);
    for (int k = 8; k < 12; ++k) expectVec(lbs[static_cast<usize>(k)], dqs[static_cast<usize>(k)], 1e-5f);
    // Blended ring: DQ preserves the ring radius better than LBS (no "candy wrapper" collapse).
    f32 rLbs = 0.0f, rDqs = 0.0f;
    glm::vec3 cL(0.0f), cD(0.0f);
    for (int k = 4; k < 8; ++k) {
        cL += lbs[static_cast<usize>(k)] * 0.25f;
        cD += dqs[static_cast<usize>(k)] * 0.25f;
    }
    for (int k = 4; k < 8; ++k) {
        rLbs += glm::length(lbs[static_cast<usize>(k)] - cL) * 0.25f;
        rDqs += glm::length(dqs[static_cast<usize>(k)] - cD) * 0.25f;
    }
    EXPECT_NEAR(rDqs, 0.2f, 2e-3f);
    EXPECT_LT(rLbs, rDqs);
}

TEST(AnimSkinning, InfluencesAreLimitedAndNormalized) {
    SkinnedMeshData m;
    m.positions.resize(2);
    std::vector<std::vector<std::pair<u16, f32>>> inf = {
        {{1, 0.1f}, {2, 0.4f}, {3, 0.05f}, {4, 0.2f}, {5, 0.15f}, {2, 0.1f}}, // duplicate joint 2
        {},
    };
    m.setInfluences(inf, 4, 7);
    ASSERT_EQ(m.joints.size(), 8u);
    EXPECT_EQ(m.joints[0], 2); // 0.5 after merge
    EXPECT_EQ(m.joints[1], 4);
    f32 sum = 0.0f;
    for (int i = 0; i < 4; ++i) sum += m.weights[static_cast<usize>(i)];
    EXPECT_NEAR(sum, 1.0f, 1e-6f);
    EXPECT_NEAR(m.weights[0], 0.5f / 0.95f, 1e-6f);
    EXPECT_EQ(m.joints[4], 7); // fallback joint
    EXPECT_FLOAT_EQ(m.weights[4], 1.0f);

    m.setInfluences(inf, 8);
    EXPECT_EQ(m.influencesPerVertex, 8u);
    sum = 0.0f;
    for (int i = 0; i < 8; ++i) sum += m.weights[static_cast<usize>(i)];
    EXPECT_NEAR(sum, 1.0f, 1e-6f);
}

TEST(AnimSkinning, DualQuatMatchesMatrix) {
    const glm::quat r = glm::angleAxis(0.7f, glm::normalize(glm::vec3(1, 2, 3)));
    const glm::vec3 t(1, -2, 3);
    const DualQuat dq = DualQuat::fromRigid(r, t);
    expectVec(dq.translation(), t, 1e-5f);
    const glm::mat4 m = Transform{t, r, glm::vec3(1)}.toMatrix();
    const glm::vec3 p(0.3f, 0.5f, -0.7f);
    expectVec(dq.transformPoint(p), glm::vec3(m * glm::vec4(p, 1)), 1e-5f);
    const DualQuat back = DualQuat::fromMatrix(m);
    expectVec(back.transformPoint(p), dq.transformPoint(p), 1e-5f);
}

TEST(AnimSkinning, DebugDrawEmitsBones) {
    const Skeleton s = makeChainSkeleton();
    Pose bind;
    bind.setBind(s);
    std::vector<Transform> model;
    localToModel(s, bind, model);
    int lines = 0;
    debugDrawSkeleton(s, model, Transform{}, [&](glm::vec3, glm::vec3, glm::vec4) { ++lines; }, {1, 1, 1, 1}, 0.0f);
    EXPECT_EQ(lines, 2);
}
