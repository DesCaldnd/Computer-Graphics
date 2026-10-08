// Глава 04: пользовательские настройки — применение без перезапуска и сохранение (docs/guide/04-cvars-quality.md).
#include <oxwald/core/scalability.hpp>
#include <oxwald/runtime/engine.hpp>
#include <oxwald/runtime/renderer.hpp>
#include <oxwald/runtime/settings.hpp>

#include <gtest/gtest.h>

#include <filesystem>

TEST(GuideSettings, GraphicsSettingsApplyWithoutRestart) {
    const auto userDir = std::filesystem::temp_directory_path() / "oxwald_guide_settings";
    std::filesystem::remove_all(userDir);

    auto rendererOwned = std::make_unique<ox::NullRenderer>();
    ox::NullRenderer* renderer = rendererOwned.get(); // считает вызовы settingsChanged()

    ox::Engine engine;
    engine.setRenderer(std::move(rendererOwned));
    ox::EngineConfig config;
    config.headless = true;
    config.userDir = userDir; // user://settings.json будет здесь
    ASSERT_TRUE(engine.init(config));

    // Меню «Графика»: берём текущие настройки, меняем, применяем.
    ox::Settings& settings = engine.settings();
    int notified = 0;
    ox::ScopedConnection c = settings.changed.connect([&](ox::SettingsCategory cat) {
        if (ox::hasCategory(cat, ox::SettingsCategory::Graphics)) ++notified;
    });

    ox::GraphicsSettings g = settings.user().graphics;
    g.quality = "Low";            // общий уровень
    g.groups = {{"Shadows", "Ultra"}}; // ... но тени на максимум
    g.vsync = false;
    g.maxFps = 144;
    g.fov = 100.0f;
    settings.setGraphics(g);      // -> cvar'ы + scalability + сигнал changed

    EXPECT_EQ(notified, 1);
    EXPECT_FALSE(ox::cvars::vsync().get());
    EXPECT_EQ(ox::cvars::maxFps().get(), 144);
    EXPECT_FLOAT_EQ(ox::cvars::fov().get(), 100.0f);
    EXPECT_EQ(ox::scalability::currentLevel(ox::Scalability::Textures), ox::QualityLevel::Low);
    EXPECT_EQ(ox::scalability::currentLevel(ox::Scalability::Shadows), ox::QualityLevel::Ultra);

    // Рендерер узнаёт об изменении на своём потоке перед следующим кадром — без перезапуска.
    engine.tick(1.0 / 60.0);
    engine.pipeline().flush();
    EXPECT_EQ(renderer->settingsChanges(), 1u);

    // Изменение r.* / sg.* из консоли движка работает так же.
    ASSERT_TRUE(engine.console().execute("sg.Shadows Medium"));
    engine.tick(1.0 / 60.0);
    engine.pipeline().flush();
    EXPECT_EQ(renderer->settingsChanges(), 2u);
    EXPECT_EQ(settings.user().graphics.groups.at("Shadows"), "Medium"); // настройки подхватили правку

    // Громкость и прочее — тем же путём.
    ox::AudioSettings a = settings.user().audio;
    a.masterVolume = 0.5f;
    a.busVolumes["Music"] = 0.3f;
    settings.setAudio(a);
    EXPECT_FLOAT_EQ(ox::cvars::masterVolume().get(), 0.5f);
    EXPECT_FLOAT_EQ(settings.busVolume("Music"), 0.3f);

    engine.shutdown(); // по умолчанию сохраняет user://settings.json
    EXPECT_TRUE(std::filesystem::exists(userDir / "settings.json"));

    // Следующий запуск подхватит сохранённое.
    ox::Engine again;
    ox::EngineConfig config2 = config;
    config2.saveUserSettingsOnShutdown = false;
    ASSERT_TRUE(again.init(config2));
    EXPECT_EQ(again.settings().user().graphics.maxFps, 144);
    EXPECT_EQ(ox::cvars::maxFps().get(), 144);
    again.shutdown();

    std::filesystem::remove_all(userDir);
    ox::cvars::vsync().reset();
    ox::cvars::maxFps().reset();
    ox::cvars::fov().reset();
    ox::cvars::masterVolume().reset();
    ox::scalability::setOverall(ox::QualityLevel::High);
}
