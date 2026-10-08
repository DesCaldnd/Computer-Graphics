// Глава 07: сохранения через Engine — saveGame/loadGame, quick save, консоль (docs/guide/07-savegames.md).
#include <oxwald/runtime/engine.hpp>
#include <oxwald/scene/scene.hpp>
#include <oxwald/scene/scene_serializer.hpp>

#include <gtest/gtest.h>

#include <filesystem>

namespace {

struct HealthComponent {
    ox::f32 health = 100.0f;
};

void registerGameTypes() {
    ox::registerSceneTypes();
    OX_REFLECT_TYPE(HealthComponent, "Game.Health").field("health", &HealthComponent::health, ox::attr::SaveGame{});
    ox::ComponentRegistry::instance().add<HealthComponent>();
}

} // namespace

TEST(GuideSaveGameEngine, SaveAndReloadLevel) {
    registerGameTypes();
    const auto dir = std::filesystem::temp_directory_path() / ("oxwald_guide_esave_" + ox::Uuid::generate().toString());
    std::filesystem::create_directories(dir);
    const auto levelPath = dir / "level.oxscene";
    {
        ox::World level;
        level.create("Hero").add<HealthComponent>();
        level.create("Barrel");
        ASSERT_TRUE(ox::saveScene(level, levelPath));
    }

    ox::EngineConfig config;
    config.headless = true;
    config.workerThreads = 2;
    config.userDir = dir / "user";          // сохранения: user://saves/<slot>.oxsave
    config.startupScene = levelPath.string();
    ox::Engine engine;
    ASSERT_TRUE(engine.init(config));

    engine.world().findByName("Hero").get<HealthComponent>().health = 5.0f;
    engine.world().destroyImmediate(engine.world().findByName("Barrel"));
    ASSERT_TRUE(engine.saveGame("slot1", "Перед боссом"));

    engine.world().findByName("Hero").get<HealthComponent>().health = 99.0f;
    auto loaded = engine.loadGame("slot1"); // перезагружает уровень из сохранения, затем применяет данные
    ASSERT_TRUE(loaded) << loaded.error().message;
    EXPECT_EQ(engine.world().findByName("Hero").get<HealthComponent>().health, 5.0f);
    EXPECT_FALSE(engine.world().findByName("Barrel").valid());

    // Быстрое сохранение (слот "quicksave") и консольная команда.
    ASSERT_TRUE(engine.saves().quickSave());
    ASSERT_TRUE(engine.console().execute("save console_slot"));
    EXPECT_TRUE(engine.saves().exists("console_slot"));
    EXPECT_TRUE(std::filesystem::exists(dir / "user/saves/quicksave.oxsave"));

    engine.shutdown();
    std::filesystem::remove_all(dir);
}
