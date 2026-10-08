// Глава 26: группы масштабируемости рендерера, уровни Low/Medium/High/Ultra и снимок RenderSettings
// (docs/guide/26-quality-settings.md).
#include "guide_quality.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>

using namespace ox;
namespace sc = ox::scalability;

namespace {

class QualityGroups : public ::testing::Test {
protected:
    void SetUp() override { guide::registerAllRenderCVars(); }
    void TearDown() override { sc::setOverall(QualityLevel::High); } // cvar'ы глобальны: вернуть как было
};

std::string cvar(const char* name) { return CVarRegistry::instance().find(name)->toString(); }

} // namespace

// Таблица «группа → cvar → значения по уровням» строится из реестра: так её строит меню настроек и отладочный
// оверлей, и так проверена таблица в главе.
TEST_F(QualityGroups, ListEveryGroupWithItsCVars) {
    usize total = 0;
    for (usize g = 0; g < kScalabilityGroupCount; ++g) {
        const Scalability group = Scalability(g);
        std::printf("## %s\n", std::string(sc::groupName(group)).c_str());
        for (ICVar* cv : sc::cvars(group)) {
            std::printf("| %s | %s | %s | %s | %s | %s |\n", cv->name().c_str(), cv->defaultString().c_str(),
                        cv->levelString(QualityLevel::Low).c_str(), cv->levelString(QualityLevel::Medium).c_str(),
                        cv->levelString(QualityLevel::High).c_str(), cv->levelString(QualityLevel::Ultra).c_str());
            EXPECT_TRUE(cv->hasLevelValues()) << cv->name();
            ++total;
        }
    }
    EXPECT_GE(sc::cvars(Scalability::Shadows).size(), 10u);
    EXPECT_GE(sc::cvars(Scalability::RayTracing).size(), 25u);
    EXPECT_GE(total, 100u);
}

TEST_F(QualityGroups, OverallLevelDrivesRenderSettings) {
    sc::setOverall(QualityLevel::Low);
    render::RenderSettings s = render::RenderSettings::fromCVars(); // снимок, который рендерер берёт раз в кадр
    EXPECT_EQ(s.csmResolution, 1024);
    EXPECT_EQ(s.csmCascades, 2);
    EXPECT_FALSE(s.pcss);
    EXPECT_EQ(s.anisotropy, 2);
    EXPECT_FLOAT_EQ(s.drawDistance, 400.0f);
    EXPECT_EQ(s.antiAliasing, 0);                  // AntiAliasing Low = без AA
    EXPECT_EQ(cvar("r.VolumetricClouds"), "false"); // cvar'ы фич живут рядом с фичами, читаются по имени
    EXPECT_EQ(cvar("r.AO.Method"), "1");            // Low: SSAO вместо GTAO

    sc::setOverall(QualityLevel::Ultra);
    s = render::RenderSettings::fromCVars();
    EXPECT_EQ(s.csmResolution, 4096);
    EXPECT_EQ(s.atlasSize, 8192);
    EXPECT_FLOAT_EQ(s.drawDistance, 0.0f);          // 0 = дальняя плоскость камеры
    EXPECT_EQ(cvar("r.Terrain.Tessellation"), "true");
    EXPECT_EQ(sc::overallLevel(), QualityLevel::Ultra);
}

TEST_F(QualityGroups, MixLevelsPerGroup) {
    sc::setOverall(QualityLevel::Medium);
    sc::setGroup(Scalability::Shadows, QualityLevel::Ultra);   // тени на максимум
    sc::setGroup(Scalability::Volumetrics, QualityLevel::Low); // облака и туман подешевле
    const render::RenderSettings s = render::RenderSettings::fromCVars();
    EXPECT_EQ(s.csmResolution, 4096);
    EXPECT_EQ(s.anisotropy, 4); // Textures остался Medium
    EXPECT_EQ(cvar("r.VolumetricFog.GridSizeZ"), "32");
    EXPECT_EQ(sc::overallLevel(), QualityLevel::Custom);

    // Ручная правка cvar'а делает группу Custom; повторный выбор уровня возвращает табличные значения.
    CVarRegistry::instance().execute("r.Shadows.CSM.Distance 400");
    EXPECT_EQ(sc::currentLevel(Scalability::Shadows), QualityLevel::Custom);
    CVarRegistry::instance().execute("sg.Shadows High"); // уровень группы — тоже cvar
    EXPECT_EQ(sc::currentLevel(Scalability::Shadows), QualityLevel::High);
    EXPECT_FLOAT_EQ(render::RenderSettings::fromCVars().csmDistance, 150.0f);
}
