// Глава 11: длина дуги, равномерная расстановка, ближайшая точка (docs/guide/11-splines.md).
#include <oxwald/spline/spline.hpp>

#include <gtest/gtest.h>

#include <glm/geometric.hpp>

using namespace ox;
using namespace ox::spline;

TEST(GuideSplinesArcLength, DistanceQueries) {
    Spline road(SplineType::CatmullRom);
    road.addPoint({0, 0, 0});
    road.addPoint({1, 0, 0}); // неравномерное расстояние между точками
    road.addPoint({10, 0, -5});
    road.addPoint({20, 0, 0});

    const f32 len = road.length();
    EXPECT_GT(len, 20.0f);

    // Параметр t НЕ пропорционален расстоянию: середина по t — не середина дороги.
    const f32 tHalf = road.distanceToT(len * 0.5f);
    EXPECT_NEAR(road.tToDistance(tHalf), len * 0.5f, 1e-3f);
    EXPECT_GT(tHalf, 1.0f);

    // Столбы каждые ~2 м: шаг подгоняется, чтобы последний попал точно в конец.
    std::vector<SplineSample> posts = road.sampleByDistance(2.0f);
    ASSERT_GE(posts.size(), 2u);
    const f32 step = road.tToDistance(posts[1].t) - road.tToDistance(posts[0].t);
    for (usize i = 1; i < posts.size(); ++i) {
        EXPECT_NEAR(road.tToDistance(posts[i].t) - road.tToDistance(posts[i - 1].t), step, 1e-3f);
    }
    EXPECT_NEAR(glm::distance(posts.back().position, glm::vec3(20, 0, 0)), 0.0f, 1e-4f);

    SplineSample mid = road.evaluateAtDistance(len * 0.5f);
    EXPECT_NEAR(mid.t, tHalf, 1e-4f);

    // Ближайшая точка кривой к игроку.
    const glm::vec3 player{10, 3, -5};
    ClosestPointResult hit = road.closestPoint(player);
    EXPECT_NEAR(hit.t, 2.0f, 0.05f);
    EXPECT_NEAR(hit.distance, glm::distance(player, hit.position), 1e-4f);

    Bounds box = road.bounds();
    EXPECT_LE(box.min.z, -5.0f);
    EXPECT_GE(box.max.x, 20.0f);

    // Перед чтением из нескольких потоков: собрать кеш заранее.
    road.rebuild();
}
