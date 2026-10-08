// Глава 01: математика — Transform, AABB, Frustum, Ray, Random (docs/guide/01-core.md).
#include <oxwald/core/math.hpp>

#include <gtest/gtest.h>

using namespace ox;

TEST(GuideCoreMath, TransformComposeAndDirections) {
    // Y вверх, «вперёд» = -Z, вращения — только кватернионы.
    Transform parent;
    parent.position = {10.0f, 0.0f, 0.0f};
    parent.rotation = glm::angleAxis(toRadians(90.0f), kWorldUp); // поворот на 90° вокруг Y

    Transform child;
    child.position = {0.0f, 0.0f, -2.0f}; // на 2 м «впереди» родителя

    const Transform world = parent * child; // = compose(parent, child)
    EXPECT_TRUE(nearlyEqual(world.position, glm::vec3(8.0f, 0.0f, 0.0f)));
    EXPECT_TRUE(nearlyEqual(parent.forward(), glm::vec3(-1.0f, 0.0f, 0.0f)));

    // Матрица и обратно, обратное преобразование.
    EXPECT_TRUE(nearlyEqual(Transform::fromMatrix(world.toMatrix()), world));
    EXPECT_TRUE(nearlyEqual(parent.inverse().transformPoint(world.position), child.position));

    // Смотрим из точки на цель: lookRotation направляет -Z на forward.
    const glm::vec3 eye{0, 2, 5}, target{0, 0, 0};
    Transform camera{eye, lookRotation(target - eye)};
    EXPECT_TRUE(nearlyEqual(camera.forward(), glm::normalize(target - eye)));
}

TEST(GuideCoreMath, BoundsFrustumAndRays) {
    AABB box; // пустой, пока в него ничего не добавили
    EXPECT_FALSE(box.valid());
    box.expand(glm::vec3{-1, 0, -1});
    box.expand(glm::vec3{1, 2, 1});
    EXPECT_EQ(box.center(), glm::vec3(0, 1, 0));
    EXPECT_EQ(box.extents(), glm::vec3(1, 1, 1));

    // Камера в (0,1,10) смотрит на -Z; reversed-Z проекция, как в рендерере.
    const glm::mat4 view = glm::lookAt(glm::vec3{0, 1, 10}, glm::vec3{0, 1, 0}, kWorldUp);
    const glm::mat4 proj = perspectiveReversedZ(toRadians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    const Frustum frustum = Frustum::fromViewProj(proj * view);
    EXPECT_TRUE(frustum.intersects(box));
    EXPECT_FALSE(frustum.intersects(AABB::fromCenterExtents({0, 1, 50}, glm::vec3{1}))); // за спиной камеры

    // Луч из камеры вперёд попадает в коробку на расстоянии 9 м.
    const Ray ray{{0, 1, 10}, {0, 0, -1}};
    auto t = intersectRayAABB(ray, box);
    ASSERT_TRUE(t);
    EXPECT_FLOAT_EQ(*t, 9.0f);
    EXPECT_EQ(ray.at(*t), glm::vec3(0, 1, 1));

    auto hit = intersectRayTriangle(ray, {-1, 0, 0}, {1, 0, 0}, {0, 3, 0});
    ASSERT_TRUE(hit);
    EXPECT_FLOAT_EQ(hit->t, 10.0f);
    EXPECT_TRUE(hit->frontFace); // треугольник против часовой стрелки, если смотреть из начала луча
}

TEST(GuideCoreMath, DeterministicRandom) {
    Random a(42), b(42);
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(a.nextU32(), b.nextU32()); // одинаковый seed — одинаковая последовательность на любой платформе
    }
    const int dice = a.rangeInt(1, 6);
    EXPECT_GE(dice, 1);
    EXPECT_LE(dice, 6);
    EXPECT_NEAR(glm::length(a.unitVector()), 1.0f, 1e-5f);
}
