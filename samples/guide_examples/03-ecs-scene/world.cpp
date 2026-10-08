// Глава 03: World, сущности, компоненты, иерархия, трансформы, клонирование (docs/guide/03-ecs-scene.md).
#include <oxwald/scene/scene.hpp>

#include <gtest/gtest.h>

#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>

namespace {

struct GuideScene : ::testing::Test {
    void SetUp() override { ox::registerSceneTypes(); } // один раз при старте (идемпотентно)
};

} // namespace

TEST_F(GuideScene, EntitiesAndComponents) {
    ox::World world;
    ox::Entity lamp = world.create("Lamp"); // Id, Name, Transform, Hierarchy, WorldTransform, Active

    auto& light = lamp.add<ox::LightComponent>();
    light.type = ox::LightType::Spot;
    light.intensity = 1200.0f; // люмены

    EXPECT_TRUE(lamp.has<ox::LightComponent>());
    if (auto* mr = lamp.tryGet<ox::MeshRendererComponent>()) {
        mr->visible = false; // не выполнится: MeshRenderer не добавлен
    }
    EXPECT_EQ(lamp.tryGet<ox::MeshRendererComponent>(), nullptr);

    // Поиск: по UUID, по EntityRef, по имени.
    const ox::Uuid id = lamp.uuid();
    EXPECT_EQ(world.find(id), lamp);
    EXPECT_EQ(world.resolve(lamp.ref()), lamp);
    EXPECT_EQ(world.findByName("Lamp"), lamp);

    // Обход всех сущностей с нужным набором компонентов (EnTT view).
    world.create("Sun").add<ox::LightComponent>().type = ox::LightType::Directional;
    int spots = 0;
    for (auto [e, l] : world.view<ox::LightComponent>().each()) {
        if (l.type == ox::LightType::Spot) ++spots;
    }
    EXPECT_EQ(spots, 1);

    lamp.remove<ox::LightComponent>();
    EXPECT_FALSE(lamp.has<ox::LightComponent>());
}

TEST_F(GuideScene, DeferredDestroy) {
    ox::World world;
    ox::Entity car = world.create("Car");
    ox::Entity wheel = world.create("Wheel", car);

    car.destroy();                       // помечает Car и всё поддерево
    EXPECT_TRUE(wheel.valid());          // ещё живы до конца кадра
    EXPECT_EQ(world.flushDestroyed(), 2u); // обычно вызывает SystemScheduler::tick
    EXPECT_FALSE(wheel.valid());
}

TEST_F(GuideScene, HierarchyAndTransforms) {
    ox::World world;
    ox::Entity car = world.create("Car");
    ox::Entity wheel = world.create("Wheel", car); // дочерняя сущность
    car.setPosition({10, 0, 0});
    car.setRotation(glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0)));
    wheel.setPosition({1, 0, 0}); // локально, относительно Car

    // Кэш мировых матриц обновляется только для «грязных» поддеревьев.
    world.updateTransforms();
    const glm::vec3 cached = glm::vec3(wheel.get<ox::WorldTransformComponent>().matrix[3]);
    EXPECT_TRUE(ox::nearlyEqual(cached, glm::vec3(10, 0, -1), 1e-4f));
    // worldPosition() считает через иерархию и не зависит от кэша.
    EXPECT_TRUE(ox::nearlyEqual(wheel.worldPosition(), glm::vec3(10, 0, -1), 1e-4f));

    // Перенос в другого родителя без «прыжка» в мире.
    ox::Entity garage = world.create("Garage");
    garage.setPosition({0, 0, 50});
    wheel.setParent(garage, /*keepWorldTransform*/ true);
    EXPECT_TRUE(ox::nearlyEqual(wheel.worldPosition(), glm::vec3(10, 0, -1), 1e-4f));
    EXPECT_EQ(wheel.parent(), garage);

    // Мировые сеттеры пересчитывают локальный трансформ.
    wheel.setWorldPosition({0, 0, 0});
    EXPECT_TRUE(ox::nearlyEqual(wheel.localTransform().position, glm::vec3(0, 0, -50), 1e-4f));

    // Порядок обхода: родитель раньше детей, порядок соседей сохраняется.
    std::vector<std::string> order;
    world.forEachInHierarchy([&](entt::entity e) { order.push_back(world.wrap(e).name()); });
    EXPECT_EQ(order, (std::vector<std::string>{"Car", "Garage", "Wheel"}));
}

TEST_F(GuideScene, DirtyFlagPitfall) {
    ox::World world;
    ox::Entity e = world.create("Box");
    world.updateTransforms();

    // НЕПРАВИЛЬНО: запись через registry().get<> не помечает трансформ грязным.
    world.registry().get<ox::TransformComponent>(e.handle()).position = {5, 0, 0};
    EXPECT_EQ(world.updateTransforms(), 0u); // кэш устарел!

    // ПРАВИЛЬНО: transform(), setPosition() или patch<>().
    e.patch<ox::TransformComponent>([](ox::TransformComponent& t) { t.position = {5, 0, 0}; });
    EXPECT_EQ(world.updateTransforms(), 1u);
    EXPECT_EQ(e.get<ox::WorldTransformComponent>().matrix[3].x, 5.0f);
}

TEST_F(GuideScene, ActiveInHierarchy) {
    ox::World world;
    ox::Entity group = world.create("Enemies");
    ox::Entity orc = world.create("Orc", group);
    group.setActive(false);
    EXPECT_TRUE(orc.active());              // собственный флаг
    EXPECT_FALSE(orc.activeInHierarchy());  // но предок выключен
}

TEST_F(GuideScene, CloneWorld) {
    ox::World edit;
    ox::Entity box = edit.create("Box");
    box.add<ox::LightComponent>().intensity = 42.0f;

    std::unique_ptr<ox::World> play = edit.clone(); // те же UUID и entt-id
    ox::Entity copy = play->find(box.uuid());
    copy.get<ox::LightComponent>().intensity = 1.0f;
    copy.setPosition({9, 9, 9});

    EXPECT_EQ(box.get<ox::LightComponent>().intensity, 42.0f); // оригинал не тронут
    EXPECT_EQ(box.localTransform().position, glm::vec3(0.0f));
}
