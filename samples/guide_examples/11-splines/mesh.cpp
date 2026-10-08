// Глава 11: экструзия меша вдоль сплайна (трубы, дороги) и debug draw (docs/guide/11-splines.md).
#include <oxwald/spline/spline_debug.hpp>
#include <oxwald/spline/spline_mesh.hpp>

#include <gtest/gtest.h>

#include <glm/geometric.hpp>

#include <cmath>

using namespace ox;
using namespace ox::spline;

TEST(GuideSplinesMesh, PipeAndRoad) {
    Spline s(SplineType::Linear);
    s.addPoint({0, 0, 0});
    s.addPoint({0, 0, -10}); // прямая 10 м вдоль −Z

    // Труба радиуса 0.3, 12 сегментов, кольца каждые 0.5 м, с крышками.
    ExtrudedMesh pipe = extrude(s, makeCircleProfile(0.3f, 12), {.spacing = 0.5f, .capStart = true, .capEnd = true});
    // rings = 21, columns = 13 (шов UV) -> 273 вершины + 2 крышки по 13.
    EXPECT_EQ(pipe.vertices.size(), 21u * 13u + 2u * 13u);
    EXPECT_EQ(pipe.indices.size(), 20u * 12u * 6u + 2u * 3u * 12u);
    for (usize i = 0; i < 21u * 13u; ++i) {
        const glm::vec3 p = pipe.vertices[i].position;
        EXPECT_NEAR(std::sqrt(p.x * p.x + p.y * p.y), 0.3f, 1e-4f); // на поверхности трубы
    }

    // Дорога шириной 6 м: плоская лента, нормали вверх, v растёт с дистанцией.
    ExtrudedMesh road = extrude(s, makeStripProfile(6.0f), {.spacing = 1.0f, .vPerUnit = 0.1f});
    for (const SplineVertex& v : road.vertices) EXPECT_NEAR(v.normal.y, 1.0f, 1e-4f);
    EXPECT_NEAR(road.vertices.back().uv.y, 1.0f, 1e-4f); // 10 м * 0.1
}

TEST(GuideSplinesMesh, DebugDraw) {
    Spline s(SplineType::Bezier);
    s.addPoint({0, 0, 0});
    s.addPoint({5, 0, -5});
    s.addPoint({10, 0, 0});

    int lines = 0;
    // В игре: [&](glm::vec3 a, glm::vec3 b, glm::vec4 c) { debugDraw.line(a, b, c); }
    drawSpline(s, [&](glm::vec3, glm::vec3, glm::vec4) { ++lines; },
               {.samplesPerSegment = 16, .frameSpacing = 2.0f});
    EXPECT_GT(lines, 2 * 16); // кривая + точки + хэндлы + кадры
}
