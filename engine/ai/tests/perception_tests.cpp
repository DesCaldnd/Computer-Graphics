#include <oxwald/ai/perception.hpp>

#include <gtest/gtest.h>

#include <cmath>

using namespace ox;
using namespace ox::ai;

namespace {

// A wall: the plane z = -5 for |x| < 3 blocks line of sight.
bool wallBlocks(const glm::vec3& a, const glm::vec3& b) {
    if ((a.z + 5.f) * (b.z + 5.f) >= 0.f) {
        return false;
    }
    const f32 t = (a.z + 5.f) / (a.z - b.z);
    const f32 x = a.x + (b.x - a.x) * t;
    return std::abs(x) < 3.f;
}

struct PerceptionFixture : ::testing::Test {
    PerceptionSystem perception;
    PerceptionListenerId guard = 0;
    void SetUp() override {
        PerceptionListenerDesc d;
        d.position = {0, 0, 0};
        d.forward = {0, 0, -1};
        d.team = 1;
        d.selfId = 1;
        guard = perception.addListener(d);
        perception.setSource({.id = 1, .position = {0, 0, 0}, .team = 1}); // the guard itself
    }
};

} // namespace

TEST_F(PerceptionFixture, SeesTargetInsideConeAndRange) {
    perception.setSource({.id = 2, .position = {2, 0, -10}, .team = 2});
    perception.update(0.1f);
    EXPECT_TRUE(perception.canSee(guard, 2));
    const auto k = perception.knowledgeOf(guard, 2, Sense::Sight);
    ASSERT_TRUE(k);
    EXPECT_EQ(k->attitude, Attitude::Hostile);
    EXPECT_GT(k->strength, 0.2f);
    EXPECT_FALSE(perception.knowledgeOf(guard, 1, Sense::Sight)) << "never perceives itself";

    // Outside range.
    perception.setSource({.id = 2, .position = {0, 0, -30}, .team = 2});
    perception.update(0.1f);
    EXPECT_FALSE(perception.canSee(guard, 2));
}

TEST_F(PerceptionFixture, ConeAndPeripheralVision) {
    perception.setSource({.id = 3, .position = {10, 0, 0}, .team = 2});   // 90° to the side, far
    perception.setSource({.id = 4, .position = {4, 0, -1.5f}, .team = 2}); // ~70°, close → peripheral
    perception.setSource({.id = 5, .position = {0, 0, 8}, .team = 2});    // behind
    perception.update(0.1f);
    EXPECT_FALSE(perception.canSee(guard, 3));
    EXPECT_TRUE(perception.canSee(guard, 4));
    EXPECT_LE(perception.knowledgeOf(guard, 4, Sense::Sight)->strength, 0.5f);
    EXPECT_FALSE(perception.canSee(guard, 5));
}

TEST_F(PerceptionFixture, LineOfSightBlockedByWall) {
    perception.setRaycast(wallBlocks);
    perception.setSource({.id = 2, .position = {0, 0, -10}, .team = 2});  // behind the wall
    perception.setSource({.id = 6, .position = {-7, 0, -10}, .team = 2}); // past the wall edge, 35° off-axis
    perception.update(0.1f);
    EXPECT_FALSE(perception.canSee(guard, 2));
    EXPECT_TRUE(perception.canSee(guard, 6));
}

TEST_F(PerceptionFixture, TeamAffiliationFiltersTargets) {
    perception.setSource({.id = 7, .position = {0, 0, -5}, .team = 1}); // friendly
    perception.setSource({.id = 8, .position = {1, 0, -5}, .team = 0}); // neutral
    perception.update(0.1f);
    EXPECT_FALSE(perception.canSee(guard, 7)) << "friendlies not detected by default";
    EXPECT_TRUE(perception.canSee(guard, 8));
    EXPECT_EQ(perception.knowledgeOf(guard, 8, Sense::Sight)->attitude, Attitude::Neutral);

    perception.attitudes().set(1, 3, Attitude::Friendly);
    perception.setSource({.id = 9, .position = {-1, 0, -5}, .team = 3});
    perception.update(0.1f);
    EXPECT_FALSE(perception.canSee(guard, 9));
    EXPECT_FALSE(perception.bestHostile(guard).has_value());
}

TEST_F(PerceptionFixture, LosesSightKeepsLastKnownPositionThenForgets) {
    std::vector<std::pair<u64, bool>> events;
    perception.setEventCallback([&](PerceptionListenerId, const PerceivedStimulus& s, bool gained) {
        events.emplace_back(s.sourceId, gained);
    });
    perception.listener(guard)->sight.forgetAfter = 2.f;
    perception.setRaycast(wallBlocks);
    perception.setSource({.id = 2, .position = {-7, 0, -10}, .team = 2});
    perception.update(0.1f);
    ASSERT_TRUE(perception.canSee(guard, 2));
    EXPECT_EQ(perception.bestHostile(guard)->sourceId, 2u);

    perception.setSource({.id = 2, .position = {0, 0, -12}, .team = 2}); // ducks behind the wall
    perception.update(0.1f);
    EXPECT_FALSE(perception.canSee(guard, 2));
    auto k = perception.knowledgeOf(guard, 2, Sense::Sight);
    ASSERT_TRUE(k) << "remembered after losing sight";
    EXPECT_EQ(k->lastKnownPosition, glm::vec3(-7, 0, -10));
    const f32 s0 = k->strength;
    perception.update(1.f);
    k = perception.knowledgeOf(guard, 2, Sense::Sight);
    ASSERT_TRUE(k);
    EXPECT_LT(k->strength, s0);
    EXPECT_NEAR(k->age, 1.1f, 1e-4f);
    perception.update(1.f);
    EXPECT_FALSE(perception.knowledgeOf(guard, 2, Sense::Sight)) << "forgotten after timeout";
    EXPECT_EQ(events, (std::vector<std::pair<u64, bool>>{{2, true}, {2, false}}));
}

TEST_F(PerceptionFixture, LoseSightRangeHysteresis) {
    perception.setSource({.id = 2, .position = {0, 0, -18}, .team = 2});
    perception.update(0.1f);
    ASSERT_TRUE(perception.canSee(guard, 2));
    perception.setSource({.id = 2, .position = {0, 0, -23}, .team = 2}); // beyond range, inside loseSightRange
    perception.update(0.1f);
    EXPECT_TRUE(perception.canSee(guard, 2));
    perception.setSource({.id = 3, .position = {1, 0, -23}, .team = 2}); // never seen → normal range applies
    perception.update(0.1f);
    EXPECT_FALSE(perception.canSee(guard, 3));
}

TEST_F(PerceptionFixture, HearsNoisesWithinRadius) {
    perception.reportNoise({.position = {5, 0, 5}, .loudness = 1.f, .radius = 10.f, .instigator = 20, .team = 2, .tag = "footstep"});
    perception.reportNoise({.position = {15, 0, 0}, .loudness = 1.f, .radius = 10.f, .instigator = 21, .team = 2});
    perception.update(0.1f);
    const auto heard = perception.knowledgeOf(guard, 20, Sense::Hearing);
    ASSERT_TRUE(heard) << "noise behind the guard is heard (no FOV for hearing)";
    EXPECT_EQ(heard->tag, "footstep");
    EXPECT_TRUE(heard->currentlySensed);
    EXPECT_NEAR(heard->strength, 1.f - std::sqrt(50.f) / 10.f, 1e-3f);
    EXPECT_FALSE(perception.knowledgeOf(guard, 21, Sense::Hearing)) << "outside noise radius";

    perception.update(0.1f);
    EXPECT_FALSE(perception.knowledgeOf(guard, 20, Sense::Hearing)->currentlySensed);
    perception.update(5.f);
    EXPECT_FALSE(perception.knowledgeOf(guard, 20, Sense::Hearing)) << "forgotten";

    // Occlusion: a quiet noise behind the wall drops below the threshold.
    perception.setRaycast(wallBlocks);
    perception.reportNoise({.position = {0, 0, -8}, .loudness = 0.2f, .radius = 10.f, .instigator = 22, .team = 2});
    perception.reportNoise({.position = {0, 0, -8}, .loudness = 1.f, .radius = 10.f, .instigator = 23, .team = 2});
    perception.update(0.1f);
    EXPECT_FALSE(perception.knowledgeOf(guard, 22, Sense::Hearing));
    ASSERT_TRUE(perception.knowledgeOf(guard, 23, Sense::Hearing));
    EXPECT_NEAR(perception.knowledgeOf(guard, 23, Sense::Hearing)->strength, 0.08f, 1e-3f);

    int lines = 0;
    perception.debugDraw(guard, [&](glm::vec3, glm::vec3, glm::vec4) { ++lines; });
    EXPECT_GT(lines, 20);
}
