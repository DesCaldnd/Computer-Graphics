// Глава 09: мир, тела, формы, слои, фиксированный шаг и интерполяция (docs/guide/09-physics.md).
#include <oxwald/physics/physics.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace ox;
using namespace ox::physics;

TEST(GuidePhysicsBasics, CrateFallsOnFloor) {
    // 1. Описание мира: слои коллизий настраиваются ДО создания мира.
    PhysicsWorldDesc desc;
    desc.workerThreads = 2; // потоки Jolt (по умолчанию: число ядер - 1)
    ObjectLayer props = *desc.layers.addLayer("Props"); // пользовательский слой
    desc.layers.setCollides(props, layers::Debris, false);
    PhysicsWorld world(desc);

    // 2. Статический пол: верхняя грань на y = 0.
    BodyDesc floor;
    floor.shape = world.shapeCache().getOrCreate(ShapeDesc::box({50.f, 0.5f, 50.f}));
    floor.position = {0.f, -0.5f, 0.f};
    floor.motionType = MotionType::Static;
    world.createBody(floor);

    // 3. Динамический ящик.
    BodyDesc crate;
    crate.shape = world.shapeCache().getOrCreate(ShapeDesc::box(glm::vec3(0.5f)));
    crate.position = {0.f, 5.f, 0.f};
    crate.rotation = glm::angleAxis(0.3f, glm::vec3(0, 1, 0)); // только кватернионы
    crate.layer = props;
    crate.mass = 20.f;        // кг; 0 = из плотности формы
    crate.restitution = 0.2f; // упругость
    crate.userData = 1234;    // обычно id сущности
    BodyHandle body = world.createBody(crate);

    world.addImpulse(body, {0.f, 0.f, 50.f}); // Н·с, применяется сразу
    for (int i = 0; i < 180; ++i) {
        world.step(1.f / 60.f);
    }

    Transform t = world.getTransform(body);
    EXPECT_NEAR(t.position.y, 0.5f, 0.05f); // лежит на полу
    EXPECT_GT(t.position.z, 0.5f);          // импульс сдвинул ящик
    EXPECT_EQ(world.getUserData(body), 1234u);
    EXPECT_NEAR(world.getMass(body), 20.f, 1e-3f);
    EXPECT_EQ(world.getLayer(body), props);
}

TEST(GuidePhysicsBasics, ShapesAndShapeCache) {
    PhysicsWorld world;

    // Составная форма (compound): «гантель» из двух сфер и бруска.
    ShapeRef dumbbell = createShape(ShapeDesc::compound({
        {ShapeDesc::sphere(0.3f), {-0.6f, 0.f, 0.f}},
        {ShapeDesc::sphere(0.3f), {0.6f, 0.f, 0.f}},
        {ShapeDesc::box({0.6f, 0.08f, 0.08f}), {}},
    }));
    EXPECT_EQ(dumbbell.type(), ShapeType::Compound);

    // Выпуклая оболочка (convex hull) из точек — октаэдр.
    ShapeRef rock = createShape(ShapeDesc::convexHull(
        {{0.5f, 0, 0}, {-0.5f, 0, 0}, {0, 0.5f, 0}, {0, -0.5f, 0}, {0, 0, 0.5f}, {0, 0, -0.5f}}));
    EXPECT_EQ(rock.type(), ShapeType::ConvexHull);

    // Карта высот (height field) 33x33 — только для статических тел.
    constexpr u32 n = 33;
    std::vector<f32> heights(n * n);
    for (u32 z = 0; z < n; ++z) {
        for (u32 x = 0; x < n; ++x) {
            heights[z * n + x] = 0.2f * std::sin(f32(x) * 0.4f);
        }
    }
    ShapeRef terrain = createShape(ShapeDesc::heightField(heights, n, {-16.f, 0.f, -16.f}, {1.f, 1.f, 1.f}));
    EXPECT_EQ(terrain.type(), ShapeType::HeightField);

    // Ошибки описания не падают, а возвращают невалидный ShapeRef + текст.
    std::string error;
    ShapeRef bad = createShape(ShapeDesc::convexHull({{0, 0, 0}}), &error);
    EXPECT_FALSE(bad);
    EXPECT_FALSE(error.empty());

    // ShapeCache: одинаковые описания -> один и тот же объект формы.
    ShapeRef a = world.shapeCache().getOrCreate(ShapeDesc::sphere(0.5f));
    ShapeRef b = world.shapeCache().getOrCreate(ShapeDesc::sphere(0.5f));
    EXPECT_EQ(a, b);

    BodyDesc ground;
    ground.shape = terrain;
    ground.motionType = MotionType::Static;
    world.createBody(ground);
    BodyDesc d;
    d.shape = dumbbell;
    d.position = {0.f, 3.f, 0.f};
    BodyHandle body = world.createBody(d);
    for (int i = 0; i < 240; ++i) {
        world.step(1.f / 60.f);
    }
    EXPECT_GT(world.getPosition(body).y, 0.f);
    EXPECT_LT(world.getPosition(body).y, 0.7f);
}

TEST(GuidePhysicsBasics, FixedStepWithInterpolation) {
    PhysicsWorld world;
    BodyDesc d;
    d.shape = createShape(ShapeDesc::sphere(0.25f));
    d.position = {0.f, 10.f, 0.f};
    BodyHandle ball = world.createBody(d);

    FixedStepper stepper(60.f);   // 60 Гц, не больше 4 шагов за кадр
    TransformInterpolator interp; // хранит предыдущее и текущее состояние
    interp.track(ball);

    // Рендер идёт с «рваным» кадром 1/45 с, физика — строго 1/60.
    Transform renderXf;
    for (int frame = 0; frame < 45; ++frame) {
        const f32 frameDt = 1.f / 45.f;
        for (u32 i = stepper.advance(frameDt); i > 0; --i) {
            world.step(stepper.fixedDt());
            interp.capture(world);
        }
        renderXf = interp.get(ball, stepper.alpha()); // это и рисуем
    }
    // ~секунда симуляции (накопитель на float: 59 или 60 шагов, остаток — в alpha()).
    EXPECT_NEAR(f32(world.stepCount()), 60.f, 1.f);
    // Интерполированная позиция лежит между предыдущей и текущей.
    const f32 prevY = interp.previous(ball)->position.y;
    const f32 curY = interp.current(ball)->position.y;
    EXPECT_LE(renderXf.position.y, prevY + 1e-4f);
    EXPECT_GE(renderXf.position.y, curY - 1e-4f);
}
