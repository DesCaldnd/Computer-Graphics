// Глава 26: автоопределение качества — отображение оценки бенчмарка в уровни и рекомендации апскейлера (CPU).
#include "guide_quality.hpp"

#include <oxwald/rhi/device_caps.hpp>

#include <gtest/gtest.h>

#include <cstdio>

using namespace ox;
using namespace ox::render;

namespace {

rhi::DeviceCaps appleCaps() {
    rhi::DeviceCaps caps;
    caps.vendor = rhi::GpuVendor::Apple;
    caps.gpuName = "Apple M4 Pro";
    return caps; // без ray query: rayTracingSupported() == false
}

class AutoDetect : public ::testing::Test {
protected:
    void SetUp() override { guide::registerAllRenderCVars(); }
    void TearDown() override {
        scalability::setOverall(QualityLevel::High);
        CVarRegistry& reg = CVarRegistry::instance();
        for (const char* n : {"r.Upscaler", "r.Upscaler.Quality", "r.ScreenPercentage"}) reg.find(n)->reset();
    }
};

} // namespace

TEST_F(AutoDetect, ScoreToLevels) {
    // Пороги: < 35 Low, < 80 Medium, < 160 High, иначе Ultra. M4 Pro набирает ≈ 112.
    auto levels = levelsForScore(112.0, /*rayTracingSupported*/ false);
    EXPECT_EQ(levels[usize(Scalability::Shadows)], QualityLevel::High);
    EXPECT_EQ(levels[usize(Scalability::GlobalIllumination)], QualityLevel::Medium); // дорогие группы — на шаг ниже
    EXPECT_EQ(levels[usize(Scalability::Volumetrics)], QualityLevel::Medium);        // (пока оценка < 250)
    EXPECT_EQ(levels[usize(Scalability::RayTracing)], QualityLevel::Low);            // нет RT — всегда Low

    levels = levelsForScore(300.0, true);
    EXPECT_EQ(levels[usize(Scalability::GlobalIllumination)], QualityLevel::Ultra);
    EXPECT_EQ(levels[usize(Scalability::RayTracing)], QualityLevel::High);
    EXPECT_EQ(levelsForScore(20.0, false)[usize(Scalability::Textures)], QualityLevel::Low);
}

TEST_F(AutoDetect, RecommendedUpscaler) {
    // Без DLSS: High/Ultra — натив + TAA, Medium — TAAU Quality (67 %), Low — FSR 1 Balanced (58 %) + TAA.
    RecommendedSettings r = recommendedSettings(appleCaps(), 112.0, /*dlssAvailable*/ false);
    std::printf("%s\n", r.rationale.c_str());
    EXPECT_EQ(r.overall, QualityLevel::High);
    EXPECT_EQ(r.upscaler, UpscalerType::Off);
    EXPECT_EQ(r.antiAliasing, 2); // TAA

    r = recommendedSettings(appleCaps(), 50.0, false);
    EXPECT_EQ(r.upscaler, UpscalerType::TAAU);
    EXPECT_EQ(r.upscalerQuality, UpscalerQuality::Quality);
    EXPECT_NEAR(r.screenPercentage, 66.7f, 0.1f);

    r = recommendedSettings(appleCaps(), 20.0, false);
    EXPECT_EQ(r.upscaler, UpscalerType::FSR1);
    EXPECT_EQ(r.upscalerQuality, UpscalerQuality::Balanced);

    // RTX + DLSS: Ultra — DLAA (Native), High — Quality, Medium — Balanced, Low — Performance.
    r = recommendedSettings(appleCaps(), 112.0, /*dlssAvailable*/ true);
    EXPECT_EQ(r.upscaler, UpscalerType::DLSS);
    EXPECT_EQ(r.upscalerQuality, UpscalerQuality::Quality);
}

TEST_F(AutoDetect, ApplyRecommendedSettings) {
    applyRecommendedSettings(recommendedSettings(appleCaps(), 50.0, false));
    const RenderSettings s = RenderSettings::fromCVars();
    EXPECT_EQ(s.upscaler, i32(UpscalerType::TAAU));
    EXPECT_EQ(s.upscalerQuality, i32(UpscalerQuality::Quality));
    EXPECT_EQ(s.antiAliasing, 2);
    EXPECT_EQ(scalability::currentLevel(Scalability::Shadows), QualityLevel::Medium);
    EXPECT_EQ(scalability::currentLevel(Scalability::GlobalIllumination), QualityLevel::Low);
}

TEST_F(AutoDetect, UpscalerAvailabilityForTheMenu) {
    // Без устройства известны только статические причины (платформа, сборка, вендор).
    for (const UpscalerAvailability& u : upscalerAvailability()) {
        std::printf("%-20s available=%d temporal=%d %s\n", u.name.c_str(), int(u.available), int(u.temporal),
                    u.reason.c_str());
    }
    EXPECT_TRUE(upscalerAvailability(UpscalerType::FSR1).available);
    EXPECT_TRUE(upscalerAvailability(UpscalerType::TAAU).temporal);
    EXPECT_FLOAT_EQ(upscalerRenderScale(UpscalerQuality::Performance), 0.5f);
#if defined(__APPLE__)
    EXPECT_FALSE(upscalerAvailability(UpscalerType::DLSS).available);
#endif
}
