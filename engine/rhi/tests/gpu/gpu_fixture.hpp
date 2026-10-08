#pragma once

#include <oxwald/core/log.hpp>
#include <oxwald/rhi/device.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <random>

namespace ox::rhi::test {

// Creates a headless device with validation layers; skips when no Vulkan device exists. Any validation error
// reported while the test ran fails it.
class GpuTest : public ::testing::Test {
protected:
    void SetUp() override {
        Device::resetValidationCounters();
        DeviceDesc desc;
        desc.appName = "ox_rhi_gpu_tests";
        desc.validation = true;
        desc.shaderOptions.cacheDirectory = std::filesystem::temp_directory_path() / "oxwald_rhi_test_shader_cache";
        customize(desc);
        std::string error;
        device = Device::create(desc, &error);
        if (!device) {
            GTEST_SKIP() << "no Vulkan device: " << error;
        }
    }

    void TearDown() override {
        if (device) {
            device->waitIdle();
            releaseResources();
            device.reset();
            EXPECT_EQ(Device::validationErrorCount(), 0u) << "Vulkan validation reported errors (see log)";
        }
    }

    virtual void customize(DeviceDesc&) {}
    virtual void releaseResources() {}

    std::unique_ptr<Device> device;
};

inline std::filesystem::path makeTempDir(const char* tag) {
    std::random_device rd;
    auto dir = std::filesystem::temp_directory_path() / std::format("oxwald_rhi_{}_{:08x}", tag, rd());
    std::filesystem::create_directories(dir);
    return dir;
}

inline void writeTextFile(const std::filesystem::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

struct Rgba8 {
    u8 r, g, b, a;
};

inline Rgba8 pixel(const std::vector<u8>& data, u32 width, u32 x, u32 y) {
    const usize i = (usize(y) * width + x) * 4;
    return {data[i], data[i + 1], data[i + 2], data[i + 3]};
}

inline bool near(u8 a, u8 b, int tol = 3) { return std::abs(int(a) - int(b)) <= tol; }

} // namespace ox::rhi::test
