// Глава 26: свой cvar в группе масштабируемости и фича рендера, которая его читает.
#include "guide_quality.hpp"

#include <oxwald/rhi/device_caps.hpp>

#include <gtest/gtest.h>

using namespace ox;
using namespace ox::render;

namespace {

// cvar'ы фичи: статические объекты рядом с кодом, который их читает.               Low    Medium High   Ultra
CVar<bool> cvRain("mygame.Rain", true, "Screen-space rain streaks", Scalability::Effects, {false, true, true, true});
CVar<int> cvRainDrops("mygame.Rain.Drops", 4096, "Rain drops per view", Scalability::Effects, {1024, 2048, 4096, 8192});
CVar<float> cvRainResolution("mygame.Rain.ResolutionScale", 1.0f, "Rain buffer scale", Scalability::Effects,
                             {0.5f, 0.5f, 1.0f, 1.0f});
CVar<float> cvRainIntensity("mygame.Rain.Intensity", 1.0f, "Artistic amount (no scalability)", 0.0f, 4.0f);

class RainFeature final : public IRenderFeature {
public:
    std::string_view name() const override { return "Rain"; } // + тумблер r.Feature.Rain
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::Translucency); }
    std::vector<std::string> cvarNames() const override {
        return {"mygame.Rain", "mygame.Rain.Drops", "mygame.Rain.ResolutionScale", "mygame.Rain.Intensity"};
    }
    // Вызывается каждый кадр: смена уровня группы перестраивает граф кадра без перезапуска.
    bool isEnabled(const RenderSettings&, const rhi::DeviceCaps&) const override { return cvRain.get(); }
    void setup(FeatureContext&) override {
        // Здесь — объявление пассов: число капель cvRainDrops.get(), размер буфера × cvRainResolution.get().
    }
};

bool rainResolved(const FeatureRegistry& features) {
    const auto resolved = features.resolve(RenderSettings::fromCVars(), rhi::DeviceCaps{});
    for (IRenderFeature* f : resolved)
        if (f->name() == "Rain") return true;
    return false;
}

} // namespace

TEST(CustomQualityCVar, GroupDrivesOwnCVarsAndFeature) {
    guide::registerAllRenderCVars();
    FeatureRegistry features; // в игре: renderer->features().emplace<RainFeature>()
    features.emplace<RainFeature>();

    scalability::setGroup(Scalability::Effects, QualityLevel::Low);
    EXPECT_FALSE(cvRain.get());
    EXPECT_EQ(cvRainDrops.get(), 1024);
    EXPECT_FALSE(rainResolved(features)); // фича выключена — её пассов нет в графе

    scalability::setGroup(Scalability::Effects, QualityLevel::Ultra);
    EXPECT_TRUE(cvRain.get());
    EXPECT_EQ(cvRainDrops.get(), 8192);
    EXPECT_TRUE(rainResolved(features));
    // Наши cvar'ы в группе рядом с cvar'ами движка (r.Particles.*, r.Water.Caustics).
    bool listed = false;
    for (ICVar* cv : scalability::cvars(Scalability::Effects)) listed = listed || cv->name() == "mygame.Rain.Drops";
    EXPECT_TRUE(listed);
    EXPECT_EQ(cvRainDrops.levelValue(QualityLevel::Medium), 2048); // подсказка в меню

    // Тумблер фичи (r.Feature.<Name>) работает поверх уровня.
    FeatureRegistry::toggle("Rain").set(false);
    EXPECT_FALSE(rainResolved(features));
    FeatureRegistry::toggle("Rain").set(true);

    // Художественные параметры (без уровней) группа не трогает.
    cvRainIntensity.set(2.0f);
    scalability::setGroup(Scalability::Effects, QualityLevel::Medium);
    EXPECT_FLOAT_EQ(cvRainIntensity.get(), 2.0f);
    EXPECT_EQ(scalability::currentLevel(Scalability::Effects), QualityLevel::Medium);
    scalability::setOverall(QualityLevel::High);
    cvRainIntensity.reset();
}
