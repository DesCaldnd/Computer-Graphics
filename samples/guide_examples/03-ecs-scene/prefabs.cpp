// Глава 03: префабы, экземпляры и overrides (docs/guide/03-ecs-scene.md).
#include <oxwald/core/serial/format.hpp>
#include <oxwald/scene/prefab.hpp>
#include <oxwald/scene/scene.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

TEST(GuideScenePrefabs, CreateInstantiateOverrideUpdate) {
    ox::registerSceneTypes();
    ox::World world;

    // 1. Собираем шаблон: фонарь с лампочкой.
    ox::Entity lamp = world.create("StreetLamp");
    lamp.add<ox::MeshRendererComponent>().mesh = ox::Uuid::fromName("lamp.mesh");
    ox::Entity bulb = world.create("Bulb", lamp);
    bulb.setPosition({0, 3, 0});
    bulb.add<ox::LightComponent>().intensity = 800.0f;

    // 2. Превращаем поддерево в префаб (Document kind "prefab"); lamp становится его экземпляром.
    ox::serial::Document prefab = ox::createPrefab(world, lamp);
    EXPECT_TRUE(lamp.get<ox::PrefabInstanceComponent>().isRoot);

    // Префаб — обычный архив: можно сохранить в файл .oxprefab и прочитать обратно.
    const auto path = std::filesystem::temp_directory_path() / ("guide_lamp_" + ox::Uuid::generate().toString() + ".oxprefab");
    ASSERT_TRUE(ox::serial::saveDocument(path, prefab, ox::serial::Format::Binary));
    auto reloaded = ox::serial::loadDocument(path); // бинарный или JSON — определяется по содержимому
    ASSERT_TRUE(reloaded);
    prefab = *reloaded;
    std::filesystem::remove(path);

    // 3. Экземпляр: новые UUID, ссылки внутри переназначены.
    auto instance = ox::instantiatePrefab(world, prefab);
    ASSERT_TRUE(instance) << instance.error().message;
    ox::Entity root = *instance;
    root.setPosition({10, 0, 0}); // трансформ корня экземпляра всегда свой, это не override
    ox::Entity iBulb = root.children()[0];

    // 4. Меняем свойство на экземпляре и фиксируем это как override.
    iBulb.get<ox::LightComponent>().intensity = 1500.0f;
    EXPECT_EQ(ox::detectOverrides(iBulb, prefab), (std::vector<std::string>{"Light.intensity"}));
    ox::recordDetectedOverrides(root, prefab); // или ox::recordOverride(iBulb, "Light.intensity")
    EXPECT_TRUE(ox::isOverridden(iBulb, "Light.intensity"));

    // 5. Новая версия префаба: правим исходный экземпляр (lamp) и применяем его к префабу.
    bulb.get<ox::LightComponent>().intensity = 400.0f;
    bulb.get<ox::LightComponent>().color = {1.0f, 0.5f, 0.2f};
    ox::serial::Document prefabV2 = ox::applyInstanceToPrefab(world, lamp, prefab);

    // 6. Распространяем изменения на все экземпляры; overrides сохраняются.
    EXPECT_EQ(ox::updatePrefabInstances(world, prefabV2), 2u);
    EXPECT_EQ(iBulb.get<ox::LightComponent>().intensity, 1500.0f);                // наш override
    EXPECT_EQ(iBulb.get<ox::LightComponent>().color, glm::vec3(1.0f, 0.5f, 0.2f)); // пришло из префаба
    EXPECT_EQ(root.localTransform().position, glm::vec3(10, 0, 0));

    // 7. Откат override к значению префаба.
    EXPECT_TRUE(ox::revertOverride(iBulb, "Light.intensity", prefabV2));
    EXPECT_EQ(iBulb.get<ox::LightComponent>().intensity, 400.0f);
}
