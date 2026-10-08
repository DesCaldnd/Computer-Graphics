// Глава 02: свой ECS-компонент — рефлексия + ComponentRegistry, сохранение сцены (docs/guide/02-reflection-serialization.md).
#include <oxwald/core/serial/format.hpp>
#include <oxwald/scene/component_registry.hpp>
#include <oxwald/scene/scene.hpp>
#include <oxwald/scene/scene_serializer.hpp>
#include <oxwald/scene/world.hpp>

#include <gtest/gtest.h>

#include <filesystem>

namespace game {

struct HealthComponent {
    float max = 100.0f;
    float current = 100.0f;
    bool invulnerable = false;
    float regenPerSecond = 0.0f;
};

void registerGameComponents() {
    OX_REFLECT_TYPE(HealthComponent, "Health")
        .attributes(ox::attr::Category{"Gameplay"}, ox::attr::Meta{"icon", "heart"})
        .field("max", &HealthComponent::max, ox::attr::Range{1.0, 10000.0})
        .field("current", &HealthComponent::current, ox::attr::SaveGame{}, ox::attr::Replicated{})
        .field("invulnerable", &HealthComponent::invulnerable)
        .field("regenPerSecond", &HealthComponent::regenPerSecond, ox::attr::DisplayName{"Regen / s"});
    // Компонент становится видимым редактору, сериализатору сцен и клонированию мира.
    ox::ComponentRegistry::instance().add<HealthComponent>();
}

} // namespace game

TEST(GuideReflection, CustomComponentInScene) {
    ox::registerSceneTypes();
    game::registerGameComponents();

    const auto* info = ox::ComponentRegistry::instance().find("Health");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->category, "Gameplay");
    EXPECT_EQ(info->icon, "heart");

    const auto path = std::filesystem::temp_directory_path() / "oxwald_guide_health.oxscene";
    {
        ox::World world;
        ox::Entity boss = world.create("Boss");
        boss.add<game::HealthComponent>(5000.0f, 4200.0f, false, 5.0f);
        ASSERT_TRUE(ox::saveScene(world, path));
    }

    ox::World loaded;
    ASSERT_TRUE(ox::loadScene(loaded, path));
    ox::Entity boss = loaded.findByName("Boss");
    ASSERT_TRUE(boss);
    const auto& hp = boss.get<game::HealthComponent>();
    EXPECT_FLOAT_EQ(hp.max, 5000.0f);
    EXPECT_FLOAT_EQ(hp.current, 4200.0f);
    EXPECT_FLOAT_EQ(hp.regenPerSecond, 5.0f);

    // То, что покажет `oxdump oxwald_guide_health.oxscene`:
    auto bytes = ox::serial::readFileBytes(path);
    ASSERT_TRUE(bytes);
    auto json = ox::serial::binaryToJson(*bytes);
    ASSERT_TRUE(json);
    EXPECT_NE(json->find("\"Health\""), std::string::npos);
    std::filesystem::remove(path);
}
