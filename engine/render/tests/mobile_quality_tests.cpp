// Android quality profile selection from DeviceCaps (no GPU needed).
#include <oxwald/render/mobile_quality.hpp>

#include <gtest/gtest.h>

#include <algorithm>

using namespace ox;
using namespace ox::render;

namespace {

rhi::DeviceCaps caps(std::string gpu, rhi::GpuVendor vendor = rhi::GpuVendor::Qualcomm) {
    rhi::DeviceCaps c;
    c.gpuName = std::move(gpu);
    c.vendor = vendor;
    return c;
}

std::string cvar(const MobileQualityProfile& p, std::string_view name) {
    for (const auto& [n, v] : p.cvars) {
        if (n == name) return v;
    }
    return {};
}

} // namespace

TEST(MobileQuality, AdrenoModelParsing) {
    EXPECT_EQ(adrenoModel("Adreno (TM) 750"), 750u);
    EXPECT_EQ(adrenoModel("Adreno (TM) 619"), 619u);
    EXPECT_EQ(adrenoModel("adreno 830"), 830u);
    EXPECT_EQ(adrenoModel("Mali-G715 Immortalis MC11"), 0u);
    EXPECT_EQ(adrenoModel("Adreno"), 0u);
}

TEST(MobileQuality, OnePlus12AdrenoIsMediumWithoutRayTracingByDefault) {
    rhi::DeviceCaps c = caps("Adreno (TM) 750");
    c.accelerationStructure = true;
    c.rayQuery = true; // Adreno 750 exposes VK_KHR_ray_query
    const MobileQualityProfile p = mobileQualityProfile(c);
    EXPECT_EQ(p.tier, MobileGpuTier::Medium);
    EXPECT_FALSE(p.emulator);
    EXPECT_EQ(p.settings.overall, QualityLevel::Medium);
    EXPECT_EQ(p.settings.upscaler, UpscalerType::FSR1);
    EXPECT_EQ(p.settings.upscalerQuality, UpscalerQuality::Quality);
    EXPECT_FALSE(p.rayTracing);
    EXPECT_EQ(cvar(p, "r.RayTracing"), "0");
    EXPECT_EQ(cvar(p, "r.GpuDriven.MeshShaders"), "0") << "Adreno has no VK_EXT_mesh_shader";
    EXPECT_EQ(p.settings.levels[usize(Scalability::RayTracing)], QualityLevel::Low);
    EXPECT_NE(p.rationale.find("thermals"), std::string::npos);
    for (QualityLevel l : p.settings.levels) EXPECT_LE(i32(l), i32(QualityLevel::Medium)) << "never above Medium";

    const MobileQualityProfile rt = mobileQualityProfile(c, {.allowRayTracing = true});
    EXPECT_TRUE(rt.rayTracing);
    EXPECT_EQ(cvar(rt, "r.RayTracing"), "1");
}

TEST(MobileQuality, RayTracingNeedsRayQuery) {
    rhi::DeviceCaps c = caps("Adreno (TM) 740");
    c.accelerationStructure = true; // but no ray query
    const MobileQualityProfile p = mobileQualityProfile(c, {.allowRayTracing = true});
    EXPECT_FALSE(p.rayTracing);
    EXPECT_EQ(p.settings.levels[usize(Scalability::RayTracing)], QualityLevel::Low);
    EXPECT_NE(p.rationale.find("unsupported"), std::string::npos);
}

TEST(MobileQuality, TiersByVendor) {
    EXPECT_EQ(mobileGpuTier(caps("Adreno (TM) 660")), MobileGpuTier::Low);
    EXPECT_EQ(mobileGpuTier(caps("Adreno (TM) 730")), MobileGpuTier::Medium);
    EXPECT_EQ(mobileGpuTier(caps("Mali-G715 Immortalis MC11", rhi::GpuVendor::Arm)), MobileGpuTier::Medium);
    EXPECT_EQ(mobileGpuTier(caps("Mali-G720", rhi::GpuVendor::Arm)), MobileGpuTier::Medium);
    EXPECT_EQ(mobileGpuTier(caps("Mali-G57 MC2", rhi::GpuVendor::Arm)), MobileGpuTier::Low);
    EXPECT_EQ(mobileGpuTier(caps("Samsung Xclipse 940", rhi::GpuVendor::Amd)), MobileGpuTier::Medium);
    EXPECT_EQ(mobileGpuTier(caps("PowerVR B-Series BXM-8-256", rhi::GpuVendor::ImgTec)), MobileGpuTier::Low);
}

TEST(MobileQuality, EmulatorAndSoftwareRenderersAreLow) {
    rhi::DeviceCaps gfx = caps("NVIDIA GeForce RTX 3080", rhi::GpuVendor::Nvidia);
    gfx.driverName = "gfxstream";
    gfx.meshShader = gfx.taskShader = true;
    const MobileQualityProfile p = mobileQualityProfile(gfx);
    EXPECT_TRUE(p.emulator);
    EXPECT_EQ(p.tier, MobileGpuTier::Low);
    EXPECT_EQ(p.settings.overall, QualityLevel::Low);
    EXPECT_EQ(p.settings.upscalerQuality, UpscalerQuality::Balanced);
    EXPECT_TRUE(cvar(p, "r.GpuDriven.MeshShaders").empty()) << "mesh shaders are left alone when supported";
    EXPECT_TRUE(isEmulatorGpu(caps("SwiftShader Device (Subzero)", rhi::GpuVendor::Unknown)));
    EXPECT_TRUE(isEmulatorGpu(caps("Android Emulator OpenGL ES Translator", rhi::GpuVendor::Unknown)));
    EXPECT_GT(p.settings.screenPercentage, 40.0f);
    EXPECT_LT(p.settings.screenPercentage, 100.0f);
}
