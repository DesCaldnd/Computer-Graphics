// Глава 04: группы масштабируемости и уровни качества Low/Medium/High/Ultra (docs/guide/04-cvars-quality.md).
#include <oxwald/core/cvar.hpp>
#include <oxwald/core/scalability.hpp>

#include <gtest/gtest.h>

namespace {

// Фича «трава» объявляет свои настройки и значения для каждого уровня:     Low   Medium  High   Ultra
ox::CVar<float> cvGrassDensity("fx.Grass.Density", 1.0f, "Grass instances per m2",
                               ox::Scalability::Foliage, {0.25f, 0.5f, 1.0f, 2.0f});
ox::CVar<float> cvGrassDistance("fx.Grass.Distance", 80.0f, "Grass draw distance (m)",
                                ox::Scalability::Foliage, {30.0f, 50.0f, 80.0f, 150.0f});
ox::CVar<bool> cvGrassShadows("fx.Grass.Shadows", false, "Grass casts shadows",
                              ox::Scalability::Foliage, {false, false, false, true});
ox::CVar<int> cvDecalLimit("fx.Decals.Max", 256, "Max decals", ox::Scalability::Effects, {64, 128, 256, 512});

} // namespace

TEST(GuideScalability, LevelsGroupsAndCustom) {
    namespace sc = ox::scalability;

    sc::setOverall(ox::QualityLevel::Low); // все группы сразу
    EXPECT_FLOAT_EQ(cvGrassDensity.get(), 0.25f);
    EXPECT_EQ(cvDecalLimit.get(), 64);
    EXPECT_EQ(sc::overallLevel(), ox::QualityLevel::Low);

    sc::setGroup(ox::Scalability::Foliage, ox::QualityLevel::Ultra); // одна группа
    EXPECT_FLOAT_EQ(cvGrassDistance.get(), 150.0f);
    EXPECT_TRUE(cvGrassShadows.get());
    EXPECT_EQ(cvDecalLimit.get(), 64);
    EXPECT_EQ(sc::overallLevel(), ox::QualityLevel::Custom); // группы на разных уровнях

    // Ручная правка cvar'а переводит группу в Custom.
    ASSERT_TRUE(ox::CVarRegistry::instance().execute("fx.Grass.Distance 100"));
    EXPECT_EQ(sc::currentLevel(ox::Scalability::Foliage), ox::QualityLevel::Custom);

    // Уровень группы — тоже cvar: sg.<Группа>. Его можно менять из консоли и он сохраняется.
    ASSERT_TRUE(ox::CVarRegistry::instance().execute("sg.Foliage Medium"));
    EXPECT_FLOAT_EQ(cvGrassDistance.get(), 50.0f);
    EXPECT_EQ(sc::currentLevel(ox::Scalability::Foliage), ox::QualityLevel::Medium);

    // Пресет (группы + ручные правки) — для меню настроек и файлов конфигурации.
    cvDecalLimit.set(100, ox::CVarSource::Console);
    const nlohmann::json preset = sc::savePreset();
    EXPECT_EQ(preset["groups"]["Foliage"], "Medium");
    EXPECT_EQ(preset["overrides"]["fx.Decals.Max"], 100);
    sc::setOverall(ox::QualityLevel::High);
    ASSERT_TRUE(sc::loadPreset(preset));
    EXPECT_EQ(cvDecalLimit.get(), 100);
    EXPECT_FLOAT_EQ(cvGrassDensity.get(), 0.5f);

    // Таблица уровней доступна для UI («что изменится на Ultra?»).
    EXPECT_EQ(cvGrassDensity.levelValue(ox::QualityLevel::Ultra), 2.0f);
    EXPECT_EQ(sc::cvars(ox::Scalability::Foliage).size(), 3u);
    sc::setOverall(ox::QualityLevel::High);
}

TEST(GuideScalability, LateRegisteredCVarStartsAtCurrentLevel) {
    ox::scalability::setOverall(ox::QualityLevel::Medium);
    // Например, cvar из модуля/плагина, загруженного позже выбора качества.
    ox::CVar<int> lateCVar("fx.Grass.Variants", 8, "Grass mesh variants", ox::Scalability::Foliage, {2, 4, 8, 16});
    EXPECT_EQ(lateCVar.get(), 4);
    ox::scalability::setOverall(ox::QualityLevel::High);
}
