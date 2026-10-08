// Глава 00: создать проект (.oxproj), открыть его движком и прочитать файл через project:// (docs/guide/00-getting-started.md).
#include <oxwald/core/vfs.hpp>
#include <oxwald/runtime/engine.hpp>
#include <oxwald/runtime/project.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

TEST(GuideGettingStarted, CreateAndRunProject) {
    const auto root = std::filesystem::temp_directory_path() / "oxwald_guide_first_project";
    std::filesystem::remove_all(root);

    // 1. Проект: каталог с файлом MyGame.oxproj (обычный JSON).
    ox::Project project = ox::Project::create(root / "MyGame", "MyGame");
    project.settings.version = "0.1.0";
    project.settings.defaultQuality = "Medium";
    project.settings.physics.fixedRate = 60.0f;
    ASSERT_TRUE(project.save());
    ASSERT_TRUE(std::filesystem::exists(root / "MyGame" / "MyGame.oxproj"));

    // Любой файл проекта доступен движку как project://...
    std::filesystem::create_directories(root / "MyGame" / "config");
    std::ofstream(root / "MyGame" / "config" / "hello.txt") << "Привет, Oxwald!";

    // 2. Запуск движка с проектом.
    ox::EngineConfig config;
    config.projectPath = root / "MyGame";
    config.headless = true;
    config.userDir = root / "user";
    config.saveUserSettingsOnShutdown = false;

    ox::Engine engine;
    auto status = engine.init(config);
    ASSERT_TRUE(status) << status.error().message;
    EXPECT_EQ(engine.projectSettings().name, "MyGame");

    auto text = engine.vfs().readText("project://config/hello.txt");
    ASSERT_TRUE(text) << text.error().message;
    EXPECT_EQ(*text, "Привет, Oxwald!");

    engine.run(3);
    engine.shutdown();
    std::filesystem::remove_all(root);
}
