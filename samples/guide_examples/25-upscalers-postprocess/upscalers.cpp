// Глава 25: апскейлеры и сглаживание — CPU-часть (docs/guide/25-upscalers-postprocess.md). Режимы качества, mip bias,
// длина джиттера, доступность (с причинами для UI), «Auto»-рекомендация и её применение к cvar'ам, какие фичи кадра
// выбираются для AA/апскейлера. GPU не нужен.
#include <oxwald/core/cvar.hpp>
#include <oxwald/core/scalability.hpp>
#include <oxwald/render/features/postprocess/postprocess.hpp>
#include <oxwald/render/render.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace ox;
using namespace ox::render;

namespace {

rhi::DeviceCaps capsFor(rhi::GpuVendor vendor) {
    rhi::DeviceCaps c;
    c.vendor = vendor;
    c.gpuName = vendor == rhi::GpuVendor::Nvidia ? "NVIDIA GeForce RTX 4070" : "Apple M4 Pro";
    return c;
}

std::string cvarText(const char* name) { return CVarRegistry::instance().find(name)->toString(); }

std::vector<std::string> resolved(const FeatureRegistry& reg, const RenderSettings& s, const rhi::DeviceCaps& caps) {
    std::vector<std::string> out;
    for (IRenderFeature* f : reg.resolve(s, caps)) out.emplace_back(f->name());
    return out;
}

bool has(const std::vector<std::string>& v, const char* name) { return std::find(v.begin(), v.end(), name) != v.end(); }

} // namespace

// --- Шаг 2. Режимы качества: масштаб рендера, mip bias, джиттер ---

TEST(GuideUpscalers, QualityModesMath) {
    EXPECT_NEAR(upscalerRenderScale(UpscalerQuality::UltraPerformance), 0.333f, 1e-3f);
    EXPECT_FLOAT_EQ(upscalerRenderScale(UpscalerQuality::Performance), 0.5f);
    EXPECT_FLOAT_EQ(upscalerRenderScale(UpscalerQuality::Balanced), 0.58f);
    EXPECT_NEAR(upscalerRenderScale(UpscalerQuality::Quality), 0.667f, 1e-3f);
    EXPECT_FLOAT_EQ(upscalerRenderScale(UpscalerQuality::Native), 1.0f); // DLAA для DLSS

    // 4K-выход в режиме Performance: рендер в 1920×1080.
    const f32 scale = upscalerRenderScale(UpscalerQuality::Performance);
    EXPECT_EQ(u32(3840 * scale), 1920u);

    // Mip bias материалов: log2(render/output), у DLSS ещё −1 (рекомендация NVIDIA).
    EXPECT_FLOAT_EQ(upscalerMipBias(UpscalerType::FSR1, 0.5f), -1.0f);
    EXPECT_FLOAT_EQ(upscalerMipBias(UpscalerType::TAAU, 0.5f), -1.0f);
    EXPECT_FLOAT_EQ(upscalerMipBias(UpscalerType::DLSS, 0.5f), -2.0f);
    EXPECT_FLOAT_EQ(upscalerMipBias(UpscalerType::DLSS, 1.0f), -1.0f);
    EXPECT_FLOAT_EQ(upscalerMipBias(UpscalerType::Off, 0.5f), 0.0f);

    // Длина последовательности Halton-джиттера: 8 × (out/in)², в пределах [8, 64].
    EXPECT_EQ(upscalerJitterPhases(1.0f), 8u);
    EXPECT_EQ(upscalerJitterPhases(0.5f), 32u);
    EXPECT_EQ(upscalerJitterPhases(1.0f / 3.0f), 64u);

    EXPECT_STREQ(upscalerName(UpscalerType::FSR1), upscalerAvailability(UpscalerType::FSR1).name.c_str());
}

// --- Шаг 3. Доступность апскейлеров для меню настроек ---

TEST(GuideUpscalers, AvailabilityForSettingsUi) {
    for (const UpscalerAvailability& a : upscalerAvailability()) { // без устройства: только статические причины
        if (a.type == UpscalerType::DLSS) {
            EXPECT_TRUE(a.temporal);
#if !defined(OX_RENDER_HAS_DLSS)
            EXPECT_FALSE(a.available);
            EXPECT_FALSE(a.reason.empty()) << "причина для подсказки в UI";
#endif
        } else {
            EXPECT_TRUE(a.available) << a.name; // Off, FSR 1 и TAAU работают везде
        }
    }
    EXPECT_FALSE(upscalerAvailability(UpscalerType::FSR1).temporal); // пространственный
    EXPECT_TRUE(upscalerAvailability(UpscalerType::TAAU).temporal);
#if defined(__APPLE__)
    EXPECT_NE(upscalerAvailability(UpscalerType::DLSS).reason.find("Windows or Linux"), std::string::npos);
#endif
}

// --- Шаг 3. Какая фича занимает слот AA / апскейлера ---

TEST(GuideUpscalers, FeatureSelection) {
    FeatureRegistry reg;
    registerPostProcessFeatures(reg); // так делает Renderer::create
    const rhi::DeviceCaps apple = capsFor(rhi::GpuVendor::Apple);

    RenderSettings s;
    s.antiAliasing = 2; // r.AntiAliasing = TAA
    std::vector<std::string> n = resolved(reg, s, apple);
    EXPECT_TRUE(has(n, "TAA"));
    EXPECT_FALSE(has(n, "FSR1"));

    s.upscaler = i32(UpscalerType::FSR1); // FSR 1 берёт уже сглаженный TAA-кадр
    n = resolved(reg, s, apple);
    EXPECT_TRUE(has(n, "TAA"));
    EXPECT_TRUE(has(n, "FSR1"));

    s.upscaler = i32(UpscalerType::TAAU); // временной апскейлер заменяет TAA
    n = resolved(reg, s, apple);
    EXPECT_FALSE(has(n, "TAA"));
    EXPECT_TRUE(has(n, "TAAU"));

    s.upscaler = i32(UpscalerType::DLSS); // DLSS недоступен (не NVIDIA) → откат на TAAU
    n = resolved(reg, s, apple);
    EXPECT_FALSE(has(n, "DLSS"));
    EXPECT_TRUE(has(n, "TAAU"));
}

// --- Шаг 6. «Auto»: рекомендация по баллу бенчмарка и DLSS ---

TEST(GuideUpscalers, RecommendedSettings) {
    const rhi::DeviceCaps rtx = capsFor(rhi::GpuVendor::Nvidia);
    const rhi::DeviceCaps apple = capsFor(rhi::GpuVendor::Apple);

    RecommendedSettings r = recommendedSettings(rtx, 300.0, /*dlssAvailable*/ true);
    EXPECT_EQ(r.overall, QualityLevel::Ultra);
    EXPECT_EQ(r.upscaler, UpscalerType::DLSS);
    EXPECT_EQ(r.upscalerQuality, UpscalerQuality::Native); // DLAA
    EXPECT_EQ(recommendedSettings(rtx, 100.0, true).upscalerQuality, UpscalerQuality::Quality);
    EXPECT_EQ(recommendedSettings(rtx, 50.0, true).upscalerQuality, UpscalerQuality::Balanced);
    EXPECT_EQ(recommendedSettings(rtx, 20.0, true).upscalerQuality, UpscalerQuality::Performance);

    r = recommendedSettings(apple, 112.0, false); // Apple M4 Pro
    EXPECT_EQ(r.overall, QualityLevel::High);
    EXPECT_EQ(r.upscaler, UpscalerType::Off);
    EXPECT_EQ(r.antiAliasing, 2);
    r = recommendedSettings(apple, 50.0, false);
    EXPECT_EQ(r.upscaler, UpscalerType::TAAU);
    EXPECT_EQ(r.upscalerQuality, UpscalerQuality::Quality);
    r = recommendedSettings(apple, 10.0, false);
    EXPECT_EQ(r.upscaler, UpscalerType::FSR1);
    EXPECT_EQ(r.upscalerQuality, UpscalerQuality::Balanced);
    EXPECT_NEAR(r.screenPercentage, 58.0f, 1e-3f);
    EXPECT_FALSE(r.rationale.empty()); // "score 10 (Low): FSR 1 at 58 % + TAA (DLSS unavailable)"
}

TEST(GuideUpscalers, ApplyRecommendedSettingsAndCVars) {
    registerRenderCVars();
    FeatureRegistry reg;
    registerPostProcessFeatures(reg); // регистрирует r.Bloom, r.TAA.* и т. д.

    applyRecommendedSettings(recommendedSettings(capsFor(rhi::GpuVendor::Apple), 50.0, false));
    EXPECT_EQ(cvarText("r.Upscaler"), "TAAU");
    EXPECT_EQ(cvarText("r.Upscaler.Quality"), "Quality");
    EXPECT_EQ(cvarText("r.AntiAliasing"), "2");
    EXPECT_EQ(cvarText("r.Bloom"), "true");       // PostProcess = Medium
    EXPECT_EQ(cvarText("r.MotionBlur"), "true");

    // То же руками (консоль, конфиг, меню): имена значений enum-cvar'ов.
    CVarRegistry::instance().set("r.Upscaler", "FSR1");
    CVarRegistry::instance().set("r.Upscaler.Quality", "Native");
    const RenderSettings s = RenderSettings::fromCVars();
    EXPECT_EQ(s.upscaler, i32(UpscalerType::FSR1));
    EXPECT_EQ(s.upscalerQuality, i32(UpscalerQuality::Native));
    EXPECT_FALSE(CVarRegistry::instance().set("r.Upscaler.Quality", "DLAA")) << "значение называется Native";

    scalability::setGroup(Scalability::PostProcess, QualityLevel::Low);
    EXPECT_EQ(cvarText("r.MotionBlur"), "false");
    EXPECT_EQ(cvarText("r.DepthOfField"), "false");
    EXPECT_EQ(cvarText("r.Bloom.Quality"), "1");
    scalability::setGroup(Scalability::AntiAliasing, QualityLevel::Ultra);
    EXPECT_EQ(cvarText("r.TAA.Quality"), "3");
    EXPECT_EQ(cvarText("r.AntiAliasing.Samples"), "16");

    CVarRegistry::instance().set("r.Upscaler", "Off");
    CVarRegistry::instance().set("r.Upscaler.Quality", "Quality");
    scalability::setGroup(Scalability::PostProcess, QualityLevel::High);
    scalability::setGroup(Scalability::AntiAliasing, QualityLevel::High);
}
