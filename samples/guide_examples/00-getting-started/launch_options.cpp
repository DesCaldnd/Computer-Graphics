// Глава 00: разбор командной строки OxwaldPlayer своими руками (docs/guide/00-getting-started.md).
#include <oxwald/runtime/launch.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

TEST(GuideGettingStarted, LaunchOptionsBecomeEngineConfig) {
    const auto userDir = std::filesystem::temp_directory_path() / "oxwald_guide_launch_options";
    const std::vector<std::string> args = {"--headless", "--frames", "5", "--quality", "low",
                                           "--cvar", "t.MaxFPS=0", "--user-dir", userDir.string()};

    auto options = ox::parseLaunchOptions(args);
    ASSERT_TRUE(options) << options.error().message;
    EXPECT_TRUE(options->headless);
    EXPECT_EQ(options->frames, 5u);
    EXPECT_EQ(options->quality, ox::QualityLevel::Low);

    ox::EngineConfig config = options->toEngineConfig("MyGame");
    config.saveUserSettingsOnShutdown = false;

    ox::Engine engine;
    ASSERT_TRUE(engine.init(config));
    engine.run(options->frames);
    EXPECT_EQ(engine.stats().frame + 1, 5u);
    engine.shutdown();
    std::filesystem::remove_all(userDir);

    // Неизвестный флаг — ошибка с понятным сообщением, а не молчаливое игнорирование.
    auto bad = ox::parseLaunchOptions(std::vector<std::string>{"--bogus"});
    EXPECT_FALSE(bad);
}
