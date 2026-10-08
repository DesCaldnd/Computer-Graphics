// Guide chapter 13 «ИИ»: perception (sight, hearing, memory, teams) and utility AI.
#include <oxwald/ai/perception.hpp>
#include <oxwald/ai/utility_ai.hpp>

#include <gtest/gtest.h>

#include <cmath>

using namespace ox;
using namespace ox::ai;

namespace {

// Вместо физики — стена в плоскости z = −5 при |x| < 3 (в игре: physics.raycast(...)).
bool wallBlocks(const glm::vec3& a, const glm::vec3& b) {
    if ((a.z + 5.f) * (b.z + 5.f) >= 0.f) {
        return false;
    }
    const f32 t = (a.z + 5.f) / (a.z - b.z);
    return std::abs(a.x + (b.x - a.x) * t) < 3.f;
}

constexpr u64 kGuardId = 1;
constexpr u64 kPlayerId = 100;

} // namespace

TEST(GuideAiPerception, GuardSeesHearsAndRemembers) {
    PerceptionSystem perception;
    perception.setRaycast(wallBlocks); // true = линия взгляда перекрыта

    std::vector<std::pair<u64, bool>> events;
    perception.setEventCallback([&](PerceptionListenerId, const PerceivedStimulus& s, bool gained) {
        events.emplace_back(s.sourceId, gained); // «заметил» / «забыл»
    });

    PerceptionListenerDesc desc;
    desc.position = {0, 0, 0};
    desc.forward = {0, 0, -1};
    desc.team = 1;
    desc.selfId = kGuardId; // себя не воспринимает
    desc.sight.range = 20.f;
    desc.sight.fovDegrees = 90.f;
    desc.sight.forgetAfter = 2.f;
    const PerceptionListenerId guard = perception.addListener(desc);

    // Игрок (команда 2 → враждебен команде 1) стоит в конусе зрения, сбоку от стены.
    perception.setSource({.id = kPlayerId, .position = {-7, 0, -10}, .team = 2});
    perception.update(0.1f);
    EXPECT_TRUE(perception.canSee(guard, kPlayerId));
    const auto target = perception.bestHostile(guard);
    ASSERT_TRUE(target);
    EXPECT_EQ(target->sourceId, kPlayerId);
    EXPECT_EQ(target->attitude, Attitude::Hostile);

    // Игрок спрятался за стеной: не виден, но последняя известная позиция помнится.
    perception.setSource({.id = kPlayerId, .position = {0, 0, -12}, .team = 2});
    perception.update(0.1f);
    EXPECT_FALSE(perception.canSee(guard, kPlayerId));
    const auto memory = perception.knowledgeOf(guard, kPlayerId, Sense::Sight);
    ASSERT_TRUE(memory);
    EXPECT_FALSE(memory->currentlySensed);
    EXPECT_EQ(memory->lastKnownPosition, glm::vec3(-7, 0, -10)); // → «пойти проверить»

    perception.update(2.f); // прошло больше forgetAfter
    EXPECT_FALSE(perception.knowledgeOf(guard, kPlayerId, Sense::Sight));
    EXPECT_EQ(events, (std::vector<std::pair<u64, bool>>{{kPlayerId, true}, {kPlayerId, false}}));

    // Слух: шум за спиной охранника (у слуха нет конуса), в радиусе 10 м.
    perception.reportNoise({.position = {5, 0, 5}, .loudness = 1.f, .radius = 10.f, .instigator = kPlayerId,
                            .team = 2, .tag = "footstep"});
    perception.update(0.1f);
    const auto heard = perception.knowledgeOf(guard, kPlayerId, Sense::Hearing);
    ASSERT_TRUE(heard);
    EXPECT_EQ(heard->tag, "footstep");
    EXPECT_NEAR(heard->strength, 1.f - std::sqrt(50.f) / 10.f, 1e-3f); // линейное затухание с расстоянием
}

TEST(GuideAiPerception, TeamsAndAttitudes) {
    PerceptionSystem perception;
    const PerceptionListenerId guard = perception.addListener({.team = 1});
    perception.setSource({.id = 2, .position = {0, 0, -5}, .team = 1}); // свой
    perception.setSource({.id = 3, .position = {1, 0, -5}, .team = 0}); // нейтрал (команда 0)
    perception.setSource({.id = 4, .position = {-1, 0, -5}, .team = 3});
    perception.attitudes().set(1, 3, Attitude::Friendly); // союз команд 1 и 3 (симметрично)
    perception.update(0.1f);

    EXPECT_FALSE(perception.canSee(guard, 2)) << "друзей по умолчанию не сообщаем (detectFriendly = false)";
    EXPECT_TRUE(perception.canSee(guard, 3));
    EXPECT_FALSE(perception.canSee(guard, 4));
    EXPECT_FALSE(perception.bestHostile(guard)) << "враждебных нет";
}

TEST(GuideAiUtility, PickHealOrAttack) {
    UtilityScorer brain;
    // Каждое действие — произведение «соображений» (considerations), вход 0..1 → кривая отклика → 0..1.
    brain.addAction({.name = "Heal",
                     .considerations = {{.name = "LowHealth",
                                         .input = [](const Blackboard& bb) { return 1.f - bb.getOr<f32>("health", 1.f); },
                                         .curve = {.type = ResponseCurve::Type::Polynomial, .exponent = 2.f}}}});
    brain.addAction({.name = "Attack",
                     .considerations = {{.name = "Healthy", .input = [](const Blackboard& bb) { return bb.getOr<f32>("health", 1.f); }},
                                        {.name = "EnemyNear", .input = [](const Blackboard& bb) { return bb.getOr<f32>("enemy", 0.f); }}}});

    Blackboard bb;
    bb.set("health", 0.9f);
    bb.set("enemy", 1.f);
    EXPECT_EQ(brain.actions()[*brain.best(bb)].name, "Attack");

    bb.set("health", 0.15f);
    EXPECT_EQ(brain.actions()[*brain.best(bb)].name, "Heal");

    // «Липкость»: текущий выбор получает бонус, чтобы ИИ не дёргался между близкими вариантами.
    bb.set("health", 0.5f);
    const auto choice = brain.best(bb, /*current*/ 0, /*stickiness*/ 0.5f);
    EXPECT_EQ(choice, 0u);

    for (const auto& [name, score] : brain.scoreAll(bb)) {
        EXPECT_GE(score, 0.f);
        EXPECT_LE(score, 1.f) << name;
    }
}
