// Глава 09: запросы — raycast, shape cast, overlap, closest point (docs/guide/09-physics.md).
#include <oxwald/physics/physics.hpp>

#include <gtest/gtest.h>

using namespace ox;
using namespace ox::physics;

namespace {
BodyHandle addStaticBox(PhysicsWorld& world, glm::vec3 pos, glm::vec3 half, ObjectLayer layer = layers::Auto,
                        u64 userData = 0) {
    BodyDesc d;
    d.shape = world.shapeCache().getOrCreate(ShapeDesc::box(half));
    d.position = pos;
    d.motionType = MotionType::Static;
    d.layer = layer;
    d.userData = userData;
    return world.createBody(d);
}
} // namespace

TEST(GuidePhysicsQueries, RaycastWithFilters) {
    PhysicsWorldDesc desc;
    ObjectLayer props = *desc.layers.addLayer("Props");
    PhysicsWorld world(desc);
    BodyHandle floor = addStaticBox(world, {0.f, -0.5f, 0.f}, {50.f, 0.5f, 50.f});
    BodyHandle prop = addStaticBox(world, {0.f, 2.f, 0.f}, glm::vec3(0.5f), props, 77);

    const glm::vec3 eye{0.f, 10.f, 0.f}, down{0.f, -1.f, 0.f}; // направление можно не нормировать
    if (auto hit = world.raycast(eye, down, 100.f)) {
        EXPECT_EQ(hit->body, prop);
        EXPECT_EQ(hit->userData, 77u);
        EXPECT_NEAR(hit->distance, 7.5f, 1e-3f); // верхняя грань ящика на y = 2.5
        EXPECT_NEAR(hit->normal.y, 1.f, 1e-3f);
    } else {
        FAIL() << "raycast missed";
    }

    // Исключить слой: маска — битовое поле по слоям.
    QueryFilter noProps;
    noProps.layerMask = kAllLayers & ~layerBit(props);
    EXPECT_EQ(world.raycast(eye, down, 100.f, noProps)->body, floor);

    // Игнорировать конкретные тела (например, самого стреляющего).
    BodyHandle self[] = {prop};
    QueryFilter ignoreSelf;
    ignoreSelf.ignoreBodies = self; // span: массив должен жить до конца запроса
    EXPECT_EQ(world.raycast(eye, down, 100.f, ignoreSelf)->body, floor);

    // Произвольный предикат (вызывается под блокировкой тела — не трогайте world внутри).
    QueryFilter onlyTagged;
    onlyTagged.predicate = [](BodyHandle, u64 userData) { return userData == 77; };
    EXPECT_EQ(world.raycast(eye, down, 100.f, onlyTagged)->body, prop);

    // Все попадания, отсортированные по расстоянию.
    std::vector<RayHit> all = world.raycastAll(eye, down, 100.f);
    ASSERT_EQ(all.size(), 2u);
    EXPECT_EQ(all[0].body, prop);
    EXPECT_EQ(all[1].body, floor);
}

TEST(GuidePhysicsQueries, SweepsOverlapsClosestPoint) {
    PhysicsWorld world;
    BodyHandle floor = addStaticBox(world, {0.f, -0.5f, 0.f}, {50.f, 0.5f, 50.f});
    BodyHandle crate = addStaticBox(world, {5.f, 0.5f, 0.f}, glm::vec3(0.5f));

    // Sphere cast: «толстый луч» — куда долетит сфера радиуса 0.5.
    auto sweep = world.sphereCast({0.f, 5.f, 0.f}, 0.5f, {0.f, -1.f, 0.f}, 10.f);
    ASSERT_TRUE(sweep);
    EXPECT_EQ(sweep->body, floor);
    EXPECT_NEAR(sweep->distance, 4.5f, 0.02f);

    // Box cast вдоль +X, пол игнорируем.
    QueryFilter noFloor;
    noFloor.ignoreBodies = std::span(&floor, 1);
    auto boxHit = world.boxCast({0.f, 0.5f, 0.f}, glm::vec3(0.25f), glm::quat(1, 0, 0, 0), {1.f, 0.f, 0.f}, 10.f,
                                noFloor);
    ASSERT_TRUE(boxHit);
    EXPECT_EQ(boxHit->body, crate);
    EXPECT_NEAR(boxHit->distance, 4.25f, 0.02f);

    // Overlap: кто внутри сферы / коробки.
    std::vector<BodyHandle> nearby = world.overlapSphere({5.f, 1.2f, 0.f}, 0.5f);
    ASSERT_EQ(nearby.size(), 1u);
    EXPECT_EQ(nearby[0], crate);
    EXPECT_EQ(world.overlapBox({5.f, 0.5f, 0.f}, glm::vec3(1.f), glm::quat(1, 0, 0, 0)).size(), 2u);

    // Ближайшая точка поверхности в радиусе 5 м.
    auto cp = world.closestPoint({5.f, 3.f, 0.f}, 5.f, noFloor);
    ASSERT_TRUE(cp);
    EXPECT_EQ(cp->body, crate);
    EXPECT_NEAR(cp->distance, 2.f, 0.02f);
}
