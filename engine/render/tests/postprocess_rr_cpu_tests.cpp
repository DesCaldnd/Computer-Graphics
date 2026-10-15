// CPU tests of the DLSS Ray Reconstruction gating (r.Denoiser): selection rules, upscaler slot resolution, Auto
// quality. The GPU side is in gpu/postprocess_rr_tests.cpp.
#include "features/postprocess/pp_common.hpp"

#include <oxwald/core/cvar.hpp>
#include <oxwald/render/features/postprocess/postprocess.hpp>
#include <oxwald/render/features/raytracing/raytracing.hpp>
#include <oxwald/render/render_feature.hpp>
#include <oxwald/render/render_settings.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace ox;
using namespace ox::render;

namespace {

rhi::DeviceCaps caps(rhi::GpuVendor vendor, bool rayTracing) {
    rhi::DeviceCaps c;
    c.vendor = vendor;
    c.gpuName = vendor == rhi::GpuVendor::Nvidia ? "NVIDIA GeForce RTX 3080" : "Apple M4 Pro";
    c.accelerationStructure = rayTracing;
    c.rayQuery = rayTracing;
    return c;
}

struct DenoiserScope {
    explicit DenoiserScope(const char* value) { CVarRegistry::instance().set("r.Denoiser", value, CVarSource::Code); }
    ~DenoiserScope() { CVarRegistry::instance().set("r.Denoiser", "SVGF", CVarSource::Code); }
};

} // namespace

TEST(RayReconstructionCpu, DenoiserCVarParsesNamesAndDefaultsToSvgf) {
    ICVar* cv = CVarRegistry::instance().find("r.Denoiser");
    ASSERT_NE(cv, nullptr);
    EXPECT_EQ(cv->defaultString(), "SVGF");
    EXPECT_TRUE(cv->setFromString("Auto", CVarSource::Code));
    EXPECT_EQ(pp::cvDenoiser.get(), i32(DenoiserType::Auto));
    EXPECT_TRUE(cv->setFromString("RR", CVarSource::Code)); // alias
    EXPECT_EQ(pp::cvDenoiser.get(), i32(DenoiserType::DLSSRR));
    cv->reset();
    EXPECT_EQ(pp::cvDenoiser.get(), i32(DenoiserType::SVGF));
}

TEST(RayReconstructionCpu, SelectionNeedsRayTracingNvidiaAndTheCVar) {
    RenderSettings s;
    s.rayTracing = true;
    const rhi::DeviceCaps rtx = caps(rhi::GpuVendor::Nvidia, true);
    {
        DenoiserScope d("SVGF");
        EXPECT_FALSE(rayReconstructionSelected(s, rtx));
    }
    DenoiserScope d("Auto");
    if (!pp::rayReconstructionUsable(rtx)) GTEST_SKIP() << "this build has no NGX (Ray Reconstruction never selected)";
    EXPECT_TRUE(rayReconstructionSelected(s, rtx));
    EXPECT_FALSE(rayReconstructionSelected(s, caps(rhi::GpuVendor::Apple, false))) << "MoltenVK: no RT, no NGX";
    EXPECT_FALSE(rayReconstructionSelected(s, caps(rhi::GpuVendor::Amd, true))) << "RR is NVIDIA only";
    EXPECT_FALSE(rayReconstructionSelected(s, caps(rhi::GpuVendor::Nvidia, false))) << "needs ray tracing";
    s.rayTracing = false;
    EXPECT_FALSE(rayReconstructionSelected(s, rtx)) << "r.RayTracing off";
}

TEST(RayReconstructionCpu, TakesTheUpscalerSlotAndQualityFollowsTheUpscaler) {
    const rhi::DeviceCaps rtx = caps(rhi::GpuVendor::Nvidia, true);
    if (!pp::rayReconstructionUsable(rtx)) GTEST_SKIP() << "this build has no NGX";
    FeatureRegistry reg;
    registerPostProcessFeatures(reg);
    auto names = [&](const RenderSettings& s) {
        std::vector<std::string> n;
        for (IRenderFeature* f : reg.resolve(s, rtx)) n.emplace_back(f->name());
        return n;
    };
    auto has = [](const std::vector<std::string>& v, const char* n) { return std::find(v.begin(), v.end(), n) != v.end(); };
    RenderSettings s;
    s.rayTracing = true;
    s.antiAliasing = 2;
    for (const UpscalerType u : {UpscalerType::Off, UpscalerType::FSR1, UpscalerType::DLSS, UpscalerType::TAAU}) {
        s.upscaler = i32(u);
        DenoiserScope d("DLSSRR");
        const auto n = names(s);
        EXPECT_TRUE(has(n, "DLSSRR")) << upscalerName(u);
        for (const char* other : {"DLSS", "TAAU", "TAA", "FSR1", "DLSSRRRelease"}) EXPECT_FALSE(has(n, other)) << other;
        EXPECT_TRUE(pp::temporalUpscalerActive(s, rtx));
    }
    s.upscaler = i32(UpscalerType::Off);
    EXPECT_EQ(pp::rayReconstructionQuality(s), UpscalerQuality::Native) << "no upscaler → DLAA";
    s.upscaler = i32(UpscalerType::DLSS);
    s.upscalerQuality = i32(UpscalerQuality::Performance);
    EXPECT_EQ(pp::rayReconstructionQuality(s), UpscalerQuality::Performance);
    // SVGF: the configured upscaler runs again.
    DenoiserScope d("SVGF");
    const auto n = names(s);
    EXPECT_FALSE(has(n, "DLSSRR"));
    EXPECT_TRUE(has(n, "DLSS"));
}

TEST(RayReconstructionCpu, AutoQualitySelectsAutoDenoiser) {
    const RecommendedSettings r = recommendedSettings(caps(rhi::GpuVendor::Nvidia, true), 150.0, true);
    EXPECT_EQ(r.denoiser, DenoiserType::Auto); // falls back to SVGF wherever RR cannot run
    EXPECT_EQ(recommendedSettings(caps(rhi::GpuVendor::Apple, false), 150.0, false).denoiser, DenoiserType::Auto);
}

TEST(RayReconstructionCpu, AvailabilityWithoutDeviceHasAReason) {
    const RayReconstructionAvailability a = rayReconstructionAvailability(nullptr);
    EXPECT_FALSE(a.available);
    EXPECT_FALSE(a.reason.empty());
}
