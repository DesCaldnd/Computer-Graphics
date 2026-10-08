// Глава 11: PathFollower — движение с постоянной скоростью, режимы зацикливания, события (docs/guide/11-splines.md).
#include <oxwald/spline/path_follower.hpp>

#include <gtest/gtest.h>

#include <glm/geometric.hpp>

#include <string>
#include <vector>

using namespace ox;
using namespace ox::spline;

TEST(GuideSplinesFollower, TramPingPong) {
    Spline rails(SplineType::Linear);
    rails.addPoint({0, 0, 0});
    rails.addPoint({100, 0, 0});
    rails.addMarker("station", 0.5f); // маркер на сплайне (по t) = 50 м

    PathFollower tram(FollowerSettings{.speed = 10.0f, .loopMode = LoopMode::PingPong});
    tram.addEvent("horn", 80.0f); // событие самого follower'а (по дистанции)

    std::vector<std::string> log;
    tram.setEventCallback([&](const PathEvent& e) {
        log.push_back(std::string(e.name) + (e.direction > 0 ? "+" : "-"));
    });

    for (int i = 0; i < 16 * 60; ++i) tram.advance(rails, 1.0f / 60.0f); // 160 м пути
    // Туда: station (50), horn (80); разворот в 100; обратно: horn (80), station (50), стоп на 40.
    const std::vector<std::string> expected = {"station+", "horn+", "horn-", "station-"};
    EXPECT_EQ(log, expected);
    EXPECT_EQ(tram.direction(), -1);
    EXPECT_NEAR(tram.distance(), 40.0f, 1e-2f);

    FollowerPose pose = tram.pose(rails); // в локальном пространстве сплайна
    EXPECT_NEAR(pose.position.x, 40.0f, 1e-2f);
    // faceTravelDirection: «нос» (−Z) смотрит по ходу движения (к началу, т.е. −X).
    const glm::vec3 nose = pose.rotation * glm::vec3(0, 0, -1);
    EXPECT_NEAR(nose.x, -1.0f, 1e-4f);
}

TEST(GuideSplinesFollower, OnceFinishes) {
    Spline path(SplineType::CatmullRom);
    path.addPoint({0, 0, 0});
    path.addPoint({0.3f, 0, -0.1f}); // точки неравномерно — скорость всё равно постоянна
    path.addPoint({10, 0, -3});
    path.addPoint({20, 0, 0});

    PathFollower f(FollowerSettings{.speed = 5.0f, .loopMode = LoopMode::Once});
    FollowerPose prev = f.pose(path);
    while (!f.finished()) {
        f.advance(path, 1.0f / 60.0f);
        const FollowerPose p = f.pose(path);
        if (!f.finished()) {
            EXPECT_NEAR(glm::distance(p.position, prev.position), 5.0f / 60.0f, 2e-3f);
        }
        prev = p;
    }
    EXPECT_NEAR(f.distance(), path.length(), 1e-4f);
}
