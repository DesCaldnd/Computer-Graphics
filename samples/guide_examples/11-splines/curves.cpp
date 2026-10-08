// Глава 11: типы кривых, оценка, кадры (frames), редактирование, Bézier-хелперы (docs/guide/11-splines.md).
#include <oxwald/spline/bezier.hpp>
#include <oxwald/spline/spline.hpp>

#include <gtest/gtest.h>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <cmath>

using namespace ox;
using namespace ox::spline;

TEST(GuideSplinesCurves, CatmullRomRoad) {
    Spline road(SplineType::CatmullRom);
    road.addPoint({0, 0, 0});
    road.addPoint({10, 0, -5});
    road.addPoint({20, 1, 0});
    road.addPoint({30, 0, -5});
    SplineSettings s = road.settings();
    s.catmullRomAlpha = kCatmullRomCentripetal; // 0 uniform, 0.5 centripetal (без петель), 1 chordal
    road.setSettings(s);

    // t ∈ [0, segmentCount()]; у интерполирующих типов точка i лежит в t = i.
    EXPECT_EQ(road.segmentCount(), 3u);
    const glm::vec3 p1 = road.position(1.0f);
    EXPECT_NEAR(glm::distance(p1, glm::vec3(10, 0, -5)), 0.0f, 1e-4f);

    SplineSample smp = road.evaluate(1.25f);
    // Ортонормированный кадр: tangent — вперёд, normal — «вверх», binormal — «вправо».
    EXPECT_NEAR(glm::length(smp.tangent), 1.0f, 1e-4f);
    EXPECT_NEAR(glm::dot(smp.tangent, smp.normal), 0.0f, 1e-4f);
    EXPECT_GT(smp.normal.y, 0.9f); // кадр затравлен upVector = +Y
    glm::quat q = smp.rotation();  // −Z -> tangent, +Y -> normal (как у камеры)
    const glm::vec3 fwd = q * glm::vec3(0, 0, -1);
    EXPECT_NEAR(glm::dot(fwd, smp.tangent), 1.0f, 1e-4f);
}

TEST(GuideSplinesCurves, BezierEditing) {
    Spline path(SplineType::Bezier);
    path.addPoint({0, 0, 0});
    path.addPoint({5, 0, 0}); // новые точки — HandleMode::Auto
    path.setHandleMode(0, HandleMode::Mirrored);
    path.setOutHandle(0, {1, 2, 0}); // хэндлы — смещения ОТНОСИТЕЛЬНО точки
    EXPECT_EQ(path.point(0).inHandle, glm::vec3(-1, -2, 0)); // зеркальный пересчитан

    const glm::vec3 before = path.position(0.4f);
    const u64 version = path.version();
    const usize idx = path.insertPointAt(0.4f); // разбиение де Кастельжо — форма не меняется
    EXPECT_EQ(idx, 1u);
    EXPECT_GT(path.version(), version);         // признак «пересобрать меш»
    EXPECT_NEAR(glm::distance(path.position(1.0f), before), 0.0f, 1e-4f);

    path.addMarker("checkpoint", 1.5f);
    EXPECT_FLOAT_EQ(*path.markerT("checkpoint"), 1.5f);
}

TEST(GuideSplinesCurves, BSplineAndNurbsCircle) {
    // B-spline аппроксимирует точки (кроме концов открытой кривой).
    Spline smooth(SplineType::BSpline);
    smooth.setPoints({{.position = {0, 0, 0}}, {.position = {2, 2, 0}}, {.position = {4, 0, 0}},
                      {.position = {6, 2, 0}}});
    EXPECT_NEAR(glm::distance(smooth.position(0.0f), glm::vec3(0, 0, 0)), 0.0f, 1e-5f);
    EXPECT_NEAR(glm::distance(smooth.position(smooth.maxT()), glm::vec3(6, 2, 0)), 0.0f, 1e-5f);
    // Внутренние точки кривая не проходит: до (2,2,0) остаётся заметное расстояние.
    EXPECT_GT(smooth.closestPoint({2, 2, 0}).distance, 0.1f);

    // Точная окружность NURBS: 9 точек, степень 2, веса 1, √½, 1, ...
    const f32 r = 2.0f, w = std::sqrt(0.5f);
    const glm::vec3 pts[9] = {{r, 0, 0},   {r, r, 0},   {0, r, 0}, {-r, r, 0}, {-r, 0, 0},
                              {-r, -r, 0}, {0, -r, 0}, {r, -r, 0}, {r, 0, 0}};
    std::vector<ControlPoint> cps;
    for (int i = 0; i < 9; ++i) cps.push_back({.position = pts[i], .weight = (i % 2) ? w : 1.0f});
    Spline circle(SplineType::Nurbs);
    SplineSettings s;
    s.degree = 2;
    s.knots = {0, 0, 0, 0.25f, 0.25f, 0.5f, 0.5f, 0.75f, 0.75f, 1, 1, 1};
    circle.setSettings(s);
    circle.setPoints(cps);
    for (f32 t = 0.0f; t <= circle.maxT(); t += 0.13f) {
        EXPECT_NEAR(glm::length(circle.position(t)), r, 1e-4f);
    }
    EXPECT_NEAR(circle.length(), glm::two_pi<f32>() * r, 1e-3f);
}

TEST(GuideSplinesCurves, BezierHelpers) {
    const CubicBezier b = {glm::vec3(0, 0, 0), glm::vec3(1, 2, 0), glm::vec3(3, 2, 0), glm::vec3(4, 0, 0)};
    EXPECT_NEAR(glm::distance(bezierPosition(b, 0.3f), deCasteljau(b, 0.3f)), 0.0f, 1e-5f);
    auto [left, right] = splitBezier(b, 0.5f);
    EXPECT_NEAR(glm::distance(left[3], bezierPosition(b, 0.5f)), 0.0f, 1e-5f);
    EXPECT_NEAR(glm::distance(bezierPosition(right, 0.5f), bezierPosition(b, 0.75f)), 0.0f, 1e-5f);
    const CubicBezier h = hermiteToBezier({0, 0, 0}, {3, 0, 0}, {1, 1, 0}, {0, 3, 0});
    EXPECT_NEAR(glm::distance(bezierDerivative(h, 0.0f), glm::vec3(3, 0, 0)), 0.0f, 1e-5f);
}
