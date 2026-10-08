// Guide chapter 13 «ИИ»: crowd steering, tiled rebuilds, tile cache with dynamic obstacles.
#include <oxwald/ai/nav_crowd.hpp>
#include <oxwald/ai/nav_query.hpp>
#include <oxwald/ai/nav_tile_cache.hpp>
#include <oxwald/ai/navmesh.hpp>

#include <gtest/gtest.h>

#include <algorithm>

using namespace ox;
using namespace ox::ai;

namespace {

NavMeshInput floor20x20() {
    NavMeshInput in;
    in.addQuad({-10, 0, -10}, {-10, 0, 10}, {10, 0, 10}, {10, 0, -10});
    return in;
}

NavMeshBuildSettings agentSettings() {
    NavMeshBuildSettings s;
    s.cellSize = 0.2f;
    s.cellHeight = 0.1f;
    s.agentRadius = 0.4f;
    s.agentHeight = 1.8f;
    return s;
}

} // namespace

TEST(GuideAiCrowd, TwoGroupsSwapSidesWithoutCollisions) {
    auto mesh = NavMesh::build(floor20x20(), agentSettings());
    ASSERT_NE(mesh, nullptr);
    NavCrowd crowd(*mesh, /*maxAgents*/ 32, /*maxAgentRadius*/ 1.f);
    ASSERT_TRUE(crowd.valid());

    NavAgentParams params;
    params.radius = 0.4f;
    params.maxSpeed = 3.f;
    params.obstacleAvoidanceQuality = 3; // 0..3, выше — дороже, но аккуратнее

    std::vector<NavAgent> agents;
    std::vector<glm::vec3> goals;
    for (int i = 0; i < 4; ++i) {
        const f32 z = -3.f + 2.f * static_cast<f32>(i);
        agents.push_back(crowd.addAgent({-6, 0, z}, params)); // идут слева направо…
        goals.push_back({6, 0, z});
        agents.push_back(crowd.addAgent({6, 0, z + 0.5f}, params)); // …навстречу друг другу
        goals.push_back({-6, 0, z + 0.5f});
    }
    for (usize i = 0; i < agents.size(); ++i) {
        ASSERT_TRUE(agents[i].valid());
        ASSERT_TRUE(crowd.setTarget(agents[i], goals[i])); // цель «прилипает» к навмешу
    }

    // Фиксированный шаг симуляции; позиции агентов → трансформы сущностей.
    f32 closest = 1e9f;
    for (int step = 0; step < 600; ++step) {
        crowd.update(1.f / 20.f);
        for (usize a = 0; a < agents.size(); ++a) {
            for (usize b = a + 1; b < agents.size(); ++b) {
                const glm::vec3 d = crowd.position(agents[a]) - crowd.position(agents[b]);
                closest = std::min(closest, glm::length(glm::vec2(d.x, d.z)));
            }
        }
    }
    for (usize i = 0; i < agents.size(); ++i) {
        EXPECT_TRUE(crowd.reachedTarget(agents[i], 0.6f)) << "agent " << i;
    }
    EXPECT_GT(closest, 0.5f) << "агенты расталкиваются и обходят друг друга";
}

TEST(GuideAiTiles, RebuildOnlyTilesTouchedByANewWall) {
    NavMeshBuildSettings s = agentSettings();
    s.tiled = true;   // тайловый навмеш: большие миры, стриминг, частичные пересборки
    s.tileSize = 32;  // клеток на сторону тайла (32 × 0.2 = 6.4 м)
    const NavMeshInput open = floor20x20();
    auto mesh = NavMesh::build(open, s);
    ASSERT_NE(mesh, nullptr);
    EXPECT_EQ(mesh->tileGridSize(), glm::ivec2(4, 4));
    NavQuery query(*mesh);
    EXPECT_NEAR(query.findPath({-5, 0, 0}, {5, 0, 0}).length(), 10.f, 0.2f);

    // Игрок построил стену: пересобираем только тайлы под её AABB.
    NavMeshInput withWall = open;
    const glm::vec3 wmin(-0.5f, 0.f, -10.f), wmax(0.5f, 3.f, 6.f);
    withWall.addBox(wmin, wmax);
    const i32 rebuilt = mesh->rebuildTiles(withWall, wmin, wmax);
    EXPECT_GT(rebuilt, 0);
    EXPECT_LT(rebuilt, 16);
    EXPECT_GT(query.findPath({-5, 0, 0}, {5, 0, 0}).length(), 15.f); // теперь в обход
}

TEST(GuideAiTileCache, DynamicObstaclesCarveTheNavMesh) {
    NavTileCacheSettings s;
    s.build = agentSettings();
    s.build.tileSize = 32;
    s.maxObstacles = 64;
    std::unique_ptr<NavTileCache> cache = NavTileCache::build(floor20x20(), s);
    ASSERT_NE(cache, nullptr);

    // Запросы и толпа работают с навмешем тайл-кэша как с обычным.
    NavQuery query(cache->navMesh());
    EXPECT_NEAR(query.findPath({-5, 0, 0}, {5, 0, 0}).length(), 10.f, 0.2f);

    // Упала баррикада (ящик) поперёк пути и бочка (цилиндр).
    const NavObstacleId barricade = cache->addBox({-0.5f, -0.5f, -7.f}, {0.5f, 2.f, 7.f});
    const NavObstacleId barrel = cache->addCylinder({-5.f, -0.5f, 3.f}, /*radius*/ 1.f, /*height*/ 2.f);
    ASSERT_NE(barricade, 0u);
    ASSERT_NE(barrel, 0u);
    // В игре: cache->update(dt) каждый кадр (обрабатывает часть очереди). flush() — всё сразу.
    cache->flush();
    EXPECT_EQ(cache->obstacleCount(), 2);
    EXPECT_GT(query.findPath({-5, 0, 0}, {5, 0, 0}).length(), 15.f);

    EXPECT_TRUE(cache->removeObstacle(barricade));
    EXPECT_TRUE(cache->removeObstacle(barrel));
    cache->flush();
    EXPECT_NEAR(query.findPath({-5, 0, 0}, {5, 0, 0}).length(), 10.f, 0.2f);
}
