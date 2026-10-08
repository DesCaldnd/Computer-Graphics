// Глава 00: минимальный headless-запуск движка (docs/guide/00-getting-started.md).
#include <oxwald/runtime/engine.hpp>

#include <gtest/gtest.h>

#include <filesystem>

TEST(GuideGettingStarted, HeadlessEngineRunsFrames) {
    const auto userDir = std::filesystem::temp_directory_path() / "oxwald_guide_getting_started";

    ox::Engine engine;
    ox::EngineConfig config;
    config.appName = "HelloOxwald";
    config.headless = true;           // без окна: NullRenderer
    config.userDir = userDir;         // user:// — настройки и сохранения
    config.saveUserSettingsOnShutdown = false;

    auto status = engine.init(config);
    ASSERT_TRUE(status) << status.error().message;

    engine.run(10); // 10 кадров и выход
    EXPECT_EQ(engine.stats().frame, 9u); // индекс последнего кадра, счёт с нуля

    engine.shutdown();
    std::filesystem::remove_all(userDir);
}
