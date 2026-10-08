// Глава 26: настройки графики игрока (ox::Settings / GraphicsSettings) поверх cvar'ов рендерера.
#include "guide_quality.hpp"

#include <oxwald/runtime/settings.hpp>

#include <gtest/gtest.h>

#include <filesystem>

using namespace ox;

TEST(GraphicsUserSettings, MenuValuesReachRenderer) {
    guide::registerAllRenderCVars();
    Settings settings; // в игре: engine.settings()

    GraphicsSettings g = settings.user().graphics;
    g.quality = "Medium";                                    // общий уровень
    g.groups = {{"Shadows", "Ultra"}, {"Volumetrics", "Low"}}; // поверх — отдельные группы
    g.upscaler = "TAAU";                                     // Off / FSR1 / DLSS / TAAU
    g.upscalerQuality = "Balanced";                          // UltraPerformance … Native
    g.rayTracing = true;                                     // без ray query рендерер сам выключит
    settings.setGraphics(g); // → sg.* + r.Upscaler* + сигнал changed (движок применит без перезапуска)

    const render::RenderSettings s = render::RenderSettings::fromCVars();
    EXPECT_EQ(s.csmResolution, 4096);            // Shadows Ultra
    EXPECT_EQ(s.anisotropy, 4);                  // Textures Medium
    EXPECT_EQ(s.upscaler, i32(render::UpscalerType::TAAU));
    EXPECT_EQ(s.upscalerQuality, i32(render::UpscalerQuality::Balanced));
    EXPECT_EQ(scalability::currentLevel(Scalability::Volumetrics), QualityLevel::Low);

    // Правка из консоли → captureFromCVars() → settings.json сохранит её.
    CVarRegistry::instance().execute("sg.Textures Ultra");
    settings.captureFromCVars();
    EXPECT_EQ(settings.user().graphics.quality, "Custom");
    EXPECT_EQ(settings.user().graphics.groups.at("Textures"), "Ultra");

    const auto path = std::filesystem::temp_directory_path() / "oxwald_guide_quality_settings.json";
    ASSERT_TRUE(settings.saveTo(path));
    Settings loaded;
    ASSERT_TRUE(loaded.loadFrom(path));
    EXPECT_EQ(loaded.user().graphics.upscaler, "TAAU");
    EXPECT_EQ(loaded.user().graphics.groups.at("Shadows"), "Ultra");

    // Вернуть глобальное состояние для других тестов процесса.
    GraphicsSettings def;
    settings.setGraphics(def);
}
