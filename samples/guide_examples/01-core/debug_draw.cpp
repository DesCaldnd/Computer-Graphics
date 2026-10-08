// Глава 01: отладочная отрисовка (docs/guide/01-core.md).
#include <oxwald/core/debug_draw.hpp>
#include <oxwald/core/services.hpp>

#include <gtest/gtest.h>

TEST(GuideCoreDebugDraw, ShapesLiveForTheirDuration) {
    ox::Services services;
    auto& dd = services.emplace<ox::DebugDraw>(); // в движке DebugDraw уже зарегистрирован сервисом

    // Любой поток, любая система: рисуем «на один кадр» (duration = 0)...
    dd.aabb(ox::AABB::fromCenterExtents({0, 1, 0}, {1, 1, 1}), ox::debug_color::kGreen);
    dd.arrow({0, 0, 0}, {0, 2, 0}, 0.2f, glm::vec4{1, 0.5f, 0, 1});
    // ...или на 2 секунды, поверх геометрии (depthTest = false).
    dd.sphere({3, 1, 0}, 0.5f, ox::debug_color::kRed, /*duration*/ 2.0f, /*depthTest*/ false);
    dd.text3D({0, 2.5f, 0}, "spawn point", ox::debug_color::kYellow);

    // Рендерер раз в кадр вызывает flush(dt) и забирает линии.
    dd.flush(1.0f / 60.0f);
    EXPECT_FALSE(dd.depthTestedLines().empty());
    EXPECT_FALSE(dd.overlayLines().empty());
    ASSERT_EQ(dd.texts().size(), 1u);

    dd.flush(1.0f / 60.0f); // одноразовые фигуры исчезли, сфера ещё жива
    EXPECT_TRUE(dd.depthTestedLines().empty());
    EXPECT_FALSE(dd.overlayLines().empty());
    EXPECT_TRUE(dd.texts().empty());

    dd.flush(2.5f); // время жизни сферы истекло
    EXPECT_TRUE(dd.overlayLines().empty());
}
