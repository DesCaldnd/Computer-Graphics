// CPU tests of the postprocess-upscalers area: quality modes, availability, Auto quality, volume blending,
// white balance, scalability tables, upscaler slot resolution.
#include "features/postprocess/pp_common.hpp"

#include <oxwald/core/reflect.hpp>
#include <oxwald/core/scalability.hpp>
#include <oxwald/core/serial/convert.hpp>
#include <oxwald/render/features/postprocess/postprocess.hpp>
#include <oxwald/render/render_feature.hpp>
#include <oxwald/render/render_settings.hpp>
#include <oxwald/rhi/device.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

using namespace ox;
using namespace ox::render;

namespace {

rhi::DeviceCaps capsFor(rhi::GpuVendor vendor) {
    rhi::DeviceCaps c;
    c.vendor = vendor;
    c.gpuName = vendor == rhi::GpuVendor::Nvidia ? "NVIDIA GeForce RTX 4070" : "Apple M4 Pro";
    return c;
}

struct CVarRestore {
    std::vector<std::pair<std::string, std::string>> saved;
    explicit CVarRestore(std::initializer_list<const char*> names) {
        for (const char* n : names) {
            if (ICVar* c = CVarRegistry::instance().find(n)) saved.emplace_back(n, c->toString());
        }
    }
    ~CVarRestore() {
        for (auto& [n, v] : saved) CVarRegistry::instance().set(n, v, CVarSource::Code);
    }
};

} // namespace

TEST(PostProcessCpu, UpscalerQualityModes) {
    EXPECT_NEAR(upscalerRenderScale(UpscalerQuality::UltraPerformance), 0.333f, 1e-3f);
    EXPECT_FLOAT_EQ(upscalerRenderScale(UpscalerQuality::Performance), 0.5f);
    EXPECT_FLOAT_EQ(upscalerRenderScale(UpscalerQuality::Balanced), 0.58f);
    EXPECT_NEAR(upscalerRenderScale(UpscalerQuality::Quality), 0.667f, 1e-3f);
    EXPECT_FLOAT_EQ(upscalerRenderScale(UpscalerQuality::Native), 1.0f);
    // NVIDIA: log2(render/display) - 1; AMD FSR 1 table: -0.38 (77 %), -0.58 (67 %), -0.79 (59 %), -1 (50 %).
    EXPECT_FLOAT_EQ(upscalerMipBias(UpscalerType::DLSS, 0.5f), -2.0f);
    EXPECT_FLOAT_EQ(upscalerMipBias(UpscalerType::DLSS, 1.0f), -1.0f);
    EXPECT_FLOAT_EQ(upscalerMipBias(UpscalerType::FSR1, 0.5f), -1.0f);
    EXPECT_NEAR(upscalerMipBias(UpscalerType::FSR1, 2.0f / 3.0f), -0.585f, 1e-3f);
    EXPECT_FLOAT_EQ(upscalerMipBias(UpscalerType::Off, 0.5f), 0.0f);
    EXPECT_EQ(upscalerJitterPhases(1.0f), 8u);
    EXPECT_EQ(upscalerJitterPhases(0.5f), 32u);
    EXPECT_EQ(upscalerJitterPhases(1.0f / 3.0f), 64u); // clamped (72)
}

TEST(PostProcessCpu, AvailabilityWithoutDevice) {
    const auto all = upscalerAvailability(nullptr);
    ASSERT_EQ(all.size(), 4u);
    for (const UpscalerAvailability& a : all) {
        if (a.type == UpscalerType::DLSS) {
            EXPECT_TRUE(a.temporal);
#if !defined(OX_RENDER_HAS_DLSS)
            EXPECT_FALSE(a.available);
            EXPECT_NE(a.reason.find("NVIDIA"), std::string::npos) << a.reason;
#endif
        } else {
            EXPECT_TRUE(a.available) << a.name;
            EXPECT_TRUE(a.reason.empty());
        }
    }
    EXPECT_TRUE(upscalerAvailability(UpscalerType::TAAU).temporal);
    EXPECT_FALSE(upscalerAvailability(UpscalerType::FSR1).temporal);
#if defined(__APPLE__)
    EXPECT_NE(upscalerAvailability(UpscalerType::DLSS).reason.find("Windows or Linux"), std::string::npos);
#endif
}

TEST(PostProcessCpu, RecommendedSettingsCombineScoreAndDlss) {
    const rhi::DeviceCaps rtx = capsFor(rhi::GpuVendor::Nvidia);
    const rhi::DeviceCaps apple = capsFor(rhi::GpuVendor::Apple);
    RecommendedSettings r = recommendedSettings(rtx, 300.0, true);
    EXPECT_EQ(r.overall, QualityLevel::Ultra);
    EXPECT_EQ(r.upscaler, UpscalerType::DLSS);
    EXPECT_EQ(r.upscalerQuality, UpscalerQuality::Native); // DLAA
    r = recommendedSettings(rtx, 100.0, true);
    EXPECT_EQ(r.upscaler, UpscalerType::DLSS);
    EXPECT_EQ(r.upscalerQuality, UpscalerQuality::Quality);
    r = recommendedSettings(rtx, 20.0, true);
    EXPECT_EQ(r.upscalerQuality, UpscalerQuality::Performance);

    r = recommendedSettings(apple, 112.0, false); // M4 Pro
    EXPECT_EQ(r.overall, QualityLevel::High);
    EXPECT_EQ(r.upscaler, UpscalerType::Off);
    EXPECT_EQ(r.antiAliasing, 2);
    EXPECT_FLOAT_EQ(r.screenPercentage, 100.0f);
    r = recommendedSettings(apple, 50.0, false);
    EXPECT_EQ(r.overall, QualityLevel::Medium);
    EXPECT_EQ(r.upscaler, UpscalerType::TAAU);
    EXPECT_EQ(r.upscalerQuality, UpscalerQuality::Quality);
    r = recommendedSettings(apple, 10.0, false);
    EXPECT_EQ(r.upscaler, UpscalerType::FSR1);
    EXPECT_EQ(r.levels[usize(Scalability::Shadows)], QualityLevel::Low);
    EXPECT_FALSE(r.rationale.empty());
}

TEST(PostProcessCpu, ApplyRecommendedSettingsSetsCVars) {
    registerRenderCVars();
    pp::registerCVars();
    CVarRestore restore({"r.AntiAliasing", "r.Upscaler", "r.Upscaler.Quality", "r.ScreenPercentage", "sg.PostProcess",
                         "sg.AntiAliasing", "r.MotionBlur", "r.Bloom"});
    applyRecommendedSettings(recommendedSettings(capsFor(rhi::GpuVendor::Apple), 50.0, false));
    const RenderSettings s = RenderSettings::fromCVars();
    EXPECT_EQ(s.upscaler, i32(UpscalerType::TAAU));
    EXPECT_EQ(s.upscalerQuality, i32(UpscalerQuality::Quality));
    EXPECT_EQ(s.antiAliasing, 2);
    EXPECT_TRUE(pp::cvBloom.get());       // PostProcess Medium
    EXPECT_TRUE(pp::cvMotionBlur.get());
    scalability::setGroup(Scalability::PostProcess, QualityLevel::Low);
    EXPECT_FALSE(pp::cvMotionBlur.get());
    EXPECT_FALSE(pp::cvDof.get());
    EXPECT_TRUE(pp::cvBloom.get());
    EXPECT_EQ(pp::cvBloomQuality.get(), 1);
    scalability::setGroup(Scalability::AntiAliasing, QualityLevel::Ultra);
    EXPECT_EQ(pp::cvTaaQuality.get(), 3);
    EXPECT_EQ(pp::cvTaaSamples.get(), 16);
}

TEST(PostProcessCpu, EffectiveUpscalerFallsBackWithoutDlss) {
    RenderSettings s;
    const rhi::DeviceCaps apple = capsFor(rhi::GpuVendor::Apple);
    s.upscaler = i32(UpscalerType::DLSS);
    EXPECT_EQ(pp::effectiveUpscaler(s, apple), UpscalerType::TAAU);
    EXPECT_TRUE(pp::temporalUpscalerActive(s, apple));
    s.upscaler = i32(UpscalerType::FSR1);
    EXPECT_EQ(pp::effectiveUpscaler(s, apple), UpscalerType::FSR1);
    EXPECT_FALSE(pp::temporalUpscalerActive(s, apple));
    s.upscaler = 0;
    EXPECT_EQ(pp::effectiveUpscaler(s, apple), UpscalerType::Off);
}

TEST(PostProcessCpu, UpscalerSlotAndAntiAliasingResolution) {
    FeatureRegistry reg;
    registerPostProcessFeatures(reg);
    const rhi::DeviceCaps apple = capsFor(rhi::GpuVendor::Apple);
    auto names = [&](const RenderSettings& s) {
        std::vector<std::string> n;
        for (IRenderFeature* f : reg.resolve(s, apple)) n.emplace_back(f->name());
        return n;
    };
    auto has = [](const std::vector<std::string>& v, const char* n) { return std::find(v.begin(), v.end(), n) != v.end(); };
    RenderSettings s;
    s.antiAliasing = 2;
    auto n = names(s);
    EXPECT_TRUE(has(n, "TAA"));
    EXPECT_FALSE(has(n, "TAAU"));
    EXPECT_FALSE(has(n, "FSR1"));
    s.upscaler = i32(UpscalerType::FSR1);
    n = names(s);
    EXPECT_TRUE(has(n, "TAA")); // spatial upscaler keeps TAA at render resolution
    EXPECT_TRUE(has(n, "FSR1"));
    s.upscaler = i32(UpscalerType::TAAU);
    n = names(s);
    EXPECT_FALSE(has(n, "TAA"));
    EXPECT_TRUE(has(n, "TAAU"));
    EXPECT_FALSE(has(n, "FSR1"));
    s.upscaler = i32(UpscalerType::DLSS); // no NVIDIA GPU → TAAU
    n = names(s);
    EXPECT_TRUE(has(n, "TAAU"));
    EXPECT_FALSE(has(n, "DLSS"));
    // Only one feature of the Upscale slot is ever resolved.
    const auto resolved = reg.resolve(s, apple);
    EXPECT_EQ(FeatureRegistry::at(resolved, InjectionPoint::Upscale).size(), 1u);
}

TEST(PostProcessCpu, VolumeWeightsAndBlending) {
    PostProcessVolumeSnapshot global;
    global.unbound = true;
    global.blendWeight = 1.0f;
    global.priority = 0;
    global.settings.overrideBloom = true;
    global.settings.bloomIntensity = 0.2f;
    global.settings.overrideLens = true;
    global.settings.vignetteIntensity = 0.5f;

    PostProcessVolumeSnapshot box;
    box.unbound = false;
    box.extents = glm::vec3(2.0f);
    box.blendRadius = 2.0f;
    box.priority = 1;
    box.worldToLocal = glm::inverse(glm::translate(glm::mat4(1.0f), glm::vec3(10, 0, 0)));
    box.settings.overrideBloom = true;
    box.settings.bloomIntensity = 0.6f;
    box.settings.overrideGrading = true;
    box.settings.saturation = 0.0f;

    EXPECT_FLOAT_EQ(postProcessVolumeWeight(box, {10, 0, 0}), 1.0f);  // inside
    EXPECT_FLOAT_EQ(postProcessVolumeWeight(box, {13, 0, 0}), 0.5f);  // 1 m outside of 2 m blend radius
    EXPECT_FLOAT_EQ(postProcessVolumeWeight(box, {20, 0, 0}), 0.0f);
    EXPECT_FLOAT_EQ(postProcessVolumeWeight(global, {1000, 0, 0}), 1.0f);

    PostProcessSettings base;
    base.bloomIntensity = 0.04f;
    const std::vector<PostProcessVolumeSnapshot> vols{box, global}; // priority order is not list order
    PostProcessSettings far = blendPostProcessVolumes(base, vols, {-50, 0, 0});
    EXPECT_FLOAT_EQ(far.bloomIntensity, 0.2f);
    EXPECT_FLOAT_EQ(far.vignetteIntensity, 0.5f);
    EXPECT_FLOAT_EQ(far.saturation, 1.0f);
    PostProcessSettings inside = blendPostProcessVolumes(base, vols, {10, 0, 0});
    EXPECT_FLOAT_EQ(inside.bloomIntensity, 0.6f);
    EXPECT_FLOAT_EQ(inside.saturation, 0.0f);
    EXPECT_FLOAT_EQ(inside.vignetteIntensity, 0.5f); // category not overridden by the box
    PostProcessSettings edge = blendPostProcessVolumes(base, vols, {13, 0, 0});
    EXPECT_NEAR(edge.bloomIntensity, 0.4f, 1e-5f);
    EXPECT_NEAR(edge.saturation, 0.5f, 1e-5f);
    // Rotated / scaled boxes: worldToLocal handles rotation, extents carry the scale.
    PostProcessVolumeSnapshot rotated = box;
    rotated.worldToLocal = glm::inverse(glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, 0)) *
                                        glm::mat4_cast(glm::angleAxis(glm::radians(45.0f), glm::vec3(0, 1, 0))));
    rotated.extents = glm::vec3(1.0f, 1.0f, 1.0f);
    EXPECT_FLOAT_EQ(postProcessVolumeWeight(rotated, {1.3f, 0, 0}), 1.0f); // inside the rotated unit box
    EXPECT_NEAR(postProcessVolumeWeight(rotated, {2.0f, 0, 0}), 1.0f - (std::sqrt(2.0f) - 1.0f) * std::sqrt(2.0f) / 2.0f,
                1e-4f);
}

TEST(PostProcessCpu, WhiteBalanceMatrix) {
    const glm::mat3 id = pp::whiteBalanceMatrix(6500.0f, 0.0f);
    for (int c = 0; c < 3; ++c) {
        for (int r = 0; r < 3; ++r) EXPECT_NEAR(id[c][r], c == r ? 1.0f : 0.0f, 1e-4f);
    }
    // A warm (3000 K) illuminant is neutralised: white turns blue, like UE's White Temp.
    const glm::vec3 warm = pp::whiteBalanceMatrix(3000.0f, 0.0f) * glm::vec3(1.0f);
    EXPECT_GT(warm.b, warm.r * 1.3f);
    const glm::vec3 cool = pp::whiteBalanceMatrix(10000.0f, 0.0f) * glm::vec3(1.0f);
    EXPECT_GT(cool.r, cool.b);
    const glm::vec3 magenta = pp::whiteBalanceMatrix(6500.0f, 0.5f) * glm::vec3(1.0f);
    const glm::vec3 green = pp::whiteBalanceMatrix(6500.0f, -0.5f) * glm::vec3(1.0f);
    EXPECT_GT(magenta.g, green.g); // neutralising a magenta cast adds green
}

TEST(PostProcessCpu, VolumeComponentIsReflected) {
    registerPostProcessTypes();
    const reflect::TypeInfo& t = reflect::typeOf<PostProcessVolumeComponent>();
    EXPECT_TRUE(t.registered);
    EXPECT_EQ(t.name, "PostProcessVolume");
    PostProcessVolumeComponent c;
    c.settings.bloomIntensity = 0.5f;
    const serial::Value v = serial::toValue(c);
    PostProcessVolumeComponent back;
    back.settings.bloomIntensity = 0.0f;
    serial::fromValue(v, back);
    EXPECT_FLOAT_EQ(back.settings.bloomIntensity, 0.5f);
}
