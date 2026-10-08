// Глава 26: GPU-бенчмарк качества на headless-устройстве (≈ 30 мс работы GPU). Без Vulkan тест пропускается.
#include "guide_quality.hpp"

#include <oxwald/rhi/device.hpp>

#include <gtest/gtest.h>

#include <cstdio>

using namespace ox;
using namespace ox::render;

TEST(AutoDetectGpu, BenchmarkAndApply) {
    guide::registerAllRenderCVars();
    rhi::DeviceDesc desc;
    desc.appName = "guide_quality";
    appendUpscalerVulkanExtensions(desc); // расширения NGX там, где DLSS возможен (до Device::create)
    std::string error;
    std::unique_ptr<rhi::Device> device = rhi::Device::create(desc, &error); // surface = nullptr: без окна
    if (!device) GTEST_SKIP() << "нет Vulkan-устройства: " << error;

    const BenchmarkResult bench = autoDetectQuality(*device); // fill-rate, ALU, bandwidth → score + levels
    if (!device->caps().timestampQueries) {
        EXPECT_FALSE(bench.valid); // без timestamp-запросов мерить нечем
        GTEST_SKIP() << "нет timestamp-запросов";
    }
    ASSERT_TRUE(bench.valid);
    std::printf("%s\n", bench.toString().c_str());
    EXPECT_GT(bench.fillRateGPixels, 0.0);
    EXPECT_GT(bench.aluGFlops, 0.0);
    EXPECT_GT(bench.bandwidthGBs, 0.0);
    EXPECT_GT(bench.score, 0.0);

    // Вариант 1: только уровни групп.
    applyQuality(bench);
    EXPECT_EQ(scalability::currentLevel(Scalability::Shadows), bench.levels[usize(Scalability::Shadows)]);

    // Вариант 2 (как RuntimeRendererOptions::autoDetectQuality): уровни + AA + апскейлер.
    const RecommendedSettings rec = recommendedSettings(*device, bench.score);
    std::printf("%s\n", rec.rationale.c_str());
    applyRecommendedSettings(rec);
    EXPECT_EQ(RenderSettings::fromCVars().upscaler, i32(rec.upscaler));

    scalability::setOverall(QualityLevel::High);
    for (const char* n : {"r.AntiAliasing", "r.Upscaler", "r.Upscaler.Quality", "r.ScreenPercentage"})
        CVarRegistry::instance().find(n)->reset();
}
