// Split rendering end to end. Single-GPU machines run the secondary device on the primary GPU
// (r.SplitRendering.AllowSameAdapter 1): every transfer path, the pipelining, the fallbacks and the toggling are
// exercised, only the speed-up is not real (both devices share one GPU).
#include "render_fixture.hpp"

#include <oxwald/render/features/postprocess/postprocess.hpp>
#include <oxwald/render/split_rendering.hpp>
#include <oxwald/rhi/multi_device.hpp>

#include <chrono>
#include <cstdio>

using namespace ox;
using namespace ox::render;
using namespace ox::render::test;

namespace {

struct ImageDiff {
    f64 mean = 0.0;
    u32 maxDiff = 0;
    f64 badFraction = 0.0; // > 8/255
};

ImageDiff diff(const Image& a, const Image& b) {
    ImageDiff d;
    if (a.rgba.size() != b.rgba.size() || a.rgba.empty()) {
        d.mean = 255.0;
        d.maxDiff = 255;
        d.badFraction = 1.0;
        return d;
    }
    u64 sum = 0, bad = 0;
    for (usize i = 0; i < a.rgba.size(); i += 4) {
        u32 pixelMax = 0;
        for (usize c = 0; c < 3; ++c) {
            const u32 e = u32(std::abs(int(a.rgba[i + c]) - int(b.rgba[i + c])));
            sum += e;
            pixelMax = std::max(pixelMax, e);
        }
        d.maxDiff = std::max(d.maxDiff, pixelMax);
        bad += pixelMax > 8 ? 1 : 0;
    }
    d.mean = f64(sum) / f64(a.rgba.size() / 4 * 3);
    d.badFraction = f64(bad) / f64(a.rgba.size() / 4);
    return d;
}

// Two renders of the static scene agree within this (frame-to-frame variation exists even on one GPU: dither,
// temporal noise of the shadow/AO filters).
::testing::AssertionResult similar(const Image& a, const Image& b, f64 maxMean = 0.3, f64 maxBad = 0.003) {
    const ImageDiff d = diff(a, b);
    if (d.mean <= maxMean && d.badFraction <= maxBad) return ::testing::AssertionSuccess() << "mean " << d.mean;
    return ::testing::AssertionFailure() << "mean " << d.mean << " (max " << maxMean << "), bad " << d.badFraction
                                         << " (max " << maxBad << "), max diff " << d.maxDiff;
}

bool hasSecondGpu(rhi::Device& device) {
    for (const rhi::AdapterInfo& a : rhi::enumerateAdapters(device)) {
        if (!a.primary && a.suitable && a.type != VK_PHYSICAL_DEVICE_TYPE_CPU) return true;
    }
    return false;
}

class SplitRenderTest : public RenderTest {
protected:
    // Same as RenderTest::SetUp, plus the optional extensions of the shared-memory transfer paths.
    void SetUp() override {
        registerSceneTypes();
        rhi::Device::resetValidationCounters();
        rhi::DeviceDesc desc;
        desc.appName = "ox_render_gpu_tests";
        desc.validation = true;
        desc.shaderOptions.cacheDirectory = std::filesystem::temp_directory_path() / "oxwald_render_test_shader_cache";
        appendUpscalerVulkanExtensions(desc);
        appendSplitRenderingVulkanExtensions(desc);
        std::string error;
        device = rhi::Device::create(desc, &error);
        if (!device) GTEST_SKIP() << "no Vulkan device: " << error;
        renderer = Renderer::create(*device);
        world = std::make_unique<World>();
    }
    void TearDown() override {
        RenderTest::TearDown();
        EXPECT_EQ(rhi::SecondaryDevice::liveCount(), 0u) << "secondary device leaked";
    }

    // Thin bars and wires (FXAA has work to do), static: the pipelined secondary result equals the current frame.
    void scene() {
        const Uuid bar = material({0.95f, 0.9f, 0.8f, 1.0f}, 0.0f, 0.5f);
        const Uuid ground = material({0.08f, 0.09f, 0.1f, 1.0f}, 0.0f, 0.9f);
        const Uuid red = material({0.9f, 0.15f, 0.1f, 1.0f}, 0.0f, 0.4f);
        mesh(Primitive::Plane, ground, {0, 0, 0}, glm::vec3(40.0f));
        for (int i = 0; i < 7; ++i) {
            const f32 a = glm::radians(-50.0f + 15.0f * f32(i));
            mesh(Primitive::Cube, bar, {-3.0f + f32(i), 1.5f, 0.0f}, {0.03f, 3.0f, 0.03f}, glm::angleAxis(a, glm::vec3(0, 0, 1)));
        }
        mesh(Primitive::Sphere, red, {1.5f, 0.6f, 1.0f}, glm::vec3(1.0f));
        sun({-0.3f, -0.8f, -0.5f}, 20000.0f, glm::vec3(1.0f), true);
        environment(0.5f, 1.0f);
    }
    CameraParams cam() { return camera({0, 1.6f, 6.0f}, {0, 1.5f, 0}, 12.5f, 50.0f); }

    Image renderWith(const char* mode, const char* transfer, u32 frames, u32 size = 256) {
        CVarScope m("r.SplitRendering", mode);
        CVarScope t("r.SplitRendering.Transfer", transfer);
        Options o;
        o.width = o.height = size;
        o.frames = frames;
        return render(cam(), o);
    }
};

} // namespace

TEST_F(SplitRenderTest, FallsBackWhenNoSecondaryAdapter) {
    if (hasSecondGpu(*device)) GTEST_SKIP() << "this machine has a second GPU";
    scene();
    CVarScope aa("r.AntiAliasing", "1");
    CVarScope same("r.SplitRendering.AllowSameAdapter", "false");
    const Image reference = renderWith("Off", "Auto", 3);
    const Image split = renderWith("On", "Auto", 5);
    const SplitRenderingStats s = renderer->stats().split;
    EXPECT_FALSE(s.active);
    EXPECT_EQ(s.offloaded, 0u);
    EXPECT_NE(s.status.find("no second GPU"), std::string::npos) << s.status;
    EXPECT_TRUE(similar(reference, split));
    EXPECT_EQ(rhi::SecondaryDevice::liveCount(), 0u);
}

class SplitTransferTest : public SplitRenderTest, public ::testing::WithParamInterface<const char*> {};

TEST_P(SplitTransferTest, SameAdapterMatchesSingleGpu) {
    const char* transfer = GetParam();
    scene();
    CVarScope aa("r.AntiAliasing", "1");
    CVarScope same("r.SplitRendering.AllowSameAdapter", "true");
    const Image reference = renderWith("Off", transfer, 3);
    const Image split = renderWith("On", transfer, 6);
    const SplitRenderingStats s = renderer->stats().split;
    ASSERT_TRUE(s.active) << s.status;
    EXPECT_TRUE(s.sameAdapter);
    if (std::string(transfer) != "Auto") EXPECT_EQ(s.transfer, transfer);
    EXPECT_EQ(s.segments, 1u);
    EXPECT_EQ(s.offloaded, 1u);
    EXPECT_EQ(s.imported, 1u) << "the last frame must use the secondary result";
    EXPECT_EQ(s.fallbacks, 0u);
    EXPECT_GE(s.secondaryComputeMs, 0.0);
    EXPECT_TRUE(similar(reference, split));
    GoldenResult g = compareGolden("split_rendering_fxaa", split); // one golden: every path gives the same image
    EXPECT_TRUE(g.matched) << g.message;
}

INSTANTIATE_TEST_SUITE_P(Paths, SplitTransferTest, ::testing::Values("Auto", "HostPointer", "Staging"),
                         [](const ::testing::TestParamInfo<const char*>& i) { return std::string(i.param); });

TEST_F(SplitRenderTest, ToggleAtRuntimeWithoutLeaks) {
    scene();
    CVarScope aa("r.AntiAliasing", "1");
    CVarScope same("r.SplitRendering.AllowSameAdapter", "true");
    const Image reference = renderWith("Off", "Auto", 2);
    for (int round = 0; round < 3; ++round) {
        const Image on = renderWith("On", round == 1 ? "Staging" : "Auto", 4);
        EXPECT_TRUE(renderer->stats().split.active);
        EXPECT_EQ(renderer->stats().split.imported, 1u);
        EXPECT_EQ(rhi::SecondaryDevice::liveCount(), 1u);
        EXPECT_TRUE(similar(reference, on));
        const Image off = renderWith("Off", "Auto", 2);
        EXPECT_FALSE(renderer->stats().split.active);
        EXPECT_EQ(rhi::SecondaryDevice::liveCount(), 0u) << "secondary device must be released when switched off";
        EXPECT_TRUE(similar(reference, off));
    }
    // Latency change re-creates the channel without leaking.
    {
        CVarScope lat("r.SplitRendering.Latency", "1");
        const Image l1 = renderWith("On", "Auto", 4);
        EXPECT_EQ(renderer->stats().split.latencyFrames, 1u);
        EXPECT_EQ(renderer->stats().split.imported, 1u);
        EXPECT_TRUE(similar(reference, l1));
    }
    renderWith("Off", "Auto", 1);
    EXPECT_EQ(rhi::Device::validationErrorCount(), 0u);
}

TEST_F(SplitRenderTest, DeviceLostFallsBackToPrimary) {
    scene();
    CVarScope aa("r.AntiAliasing", "1");
    CVarScope same("r.SplitRendering.AllowSameAdapter", "true");
    const Image reference = renderWith("Off", "Auto", 2);
    Options o;
    o.frames = 4;
    {
        CVarScope on("r.SplitRendering", "On");
        CVarScope lat("r.SplitRendering.Latency", "1"); // the next frame imports the job that will fail
        render(cam(), o);
        ASSERT_EQ(renderer->stats().split.imported, 1u);
        splitRenderingSimulateDeviceLost(*renderer);
        o.frames = 1;
        const Image lostFrame = render(cam(), o); // import fails: FXAA input copied through for this frame
        EXPECT_EQ(renderer->stats().split.fallbacks, 1u);
        EXPECT_TRUE(similar(reference, lostFrame, 2.0, 0.05)) << "fallback frame shows the unfiltered image";
        o.frames = 3;
        const Image after = render(cam(), o);
        const SplitRenderingStats s = splitRenderingStatus(*renderer);
        EXPECT_FALSE(s.active);
        EXPECT_NE(s.status.find("lost"), std::string::npos) << s.status;
        EXPECT_EQ(renderer->stats().split.offloaded, 0u);
        EXPECT_TRUE(similar(reference, after)) << "primary path after the loss";
        EXPECT_EQ(rhi::SecondaryDevice::liveCount(), 0u);
    }
    // Toggling the cvar retries.
    renderWith("Off", "Auto", 1);
    const Image again = renderWith("On", "Auto", 4);
    EXPECT_TRUE(renderer->stats().split.active);
    EXPECT_EQ(renderer->stats().split.imported, 1u);
    EXPECT_TRUE(similar(reference, again));
    renderWith("Off", "Auto", 1);
}

TEST_F(SplitRenderTest, AutoKeepsLightPassesOnThePrimary) {
    scene();
    CVarScope aa("r.AntiAliasing", "1");
    CVarScope same("r.SplitRendering.AllowSameAdapter", "true");
    renderWith("Auto", "Auto", 40);
    const SplitRenderingStats s = splitRenderingStatus(*renderer);
    EXPECT_TRUE(s.active) << s.status;
    EXPECT_EQ(s.segments, 1u);
    EXPECT_EQ(s.offloaded, 0u) << s.status;
    EXPECT_NE(s.status.find("Auto: "), std::string::npos) << s.status;
    EXPECT_NE(s.status.find("not profitable"), std::string::npos) << s.status;
    renderWith("Off", "Auto", 1);
}

// Overhead of the mechanism on one GPU at 1080p (both devices share the RTX 3080, so there is no speed-up to
// gain): GPU frame time of the primary, transfer passes, secondary job time, CPU time per frame.
TEST_F(SplitRenderTest, PerfReportSameAdapter1080p) {
    scene();
    CVarScope aa("r.AntiAliasing", "1");
    CVarScope same("r.SplitRendering.AllowSameAdapter", "true");
    struct Row {
        std::string name;
        f64 cpuMs, gpuMs, passMs, transferMs, secondaryMs, waitMs;
    };
    std::vector<Row> rows;
    auto passMs = [&](const char* needle) {
        f64 ms = 0.0;
        for (const PassTiming& p : renderer->stats().passes) {
            if (p.name.find(needle) != std::string::npos) ms += p.gpuMs;
        }
        return ms;
    };
    for (const char* config : {"Off", "Auto", "HostPointer", "Staging"}) {
        const bool on = std::string(config) != "Off";
        CVarScope m("r.SplitRendering", on ? "On" : "Off");
        CVarScope t("r.SplitRendering.Transfer", on ? config : "Auto");
        Options o;
        o.width = 1920;
        o.height = 1080;
        o.frames = 6; // warm up (pipelines, channel, latency)
        render(cam(), o);
        constexpr u32 kFrames = 30;
        o.frames = kFrames;
        const auto t0 = std::chrono::steady_clock::now();
        render(cam(), o);
        const f64 cpu = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count() / kFrames;
        const RenderStats& s = renderer->stats();
        rows.push_back({on ? std::string("split/") + (std::string(config) == "Auto" ? s.split.transfer : config) : "single GPU",
                        cpu, s.gpuFrameMs, passMs("/FXAA"), passMs("Split."), s.split.secondaryGpuMs,
                        s.split.waitMs});
        if (on) EXPECT_EQ(s.split.imported, 1u);
    }
    std::string table = "split rendering, same adapter (RTX 3080 runs both devices), 1080p FXAA:\n";
    table += "  config                    frame(ms) GPU(ms)  FXAA(ms) Split.*(ms) secondary(ms) wait(ms)\n";
    for (const Row& r : rows) {
        table += std::format("  {:<25} {:8.3f} {:8.3f} {:8.3f} {:10.3f} {:12.3f} {:8.3f}\n", r.name, r.cpuMs, r.gpuMs,
                             r.passMs, r.transferMs, r.secondaryMs, r.waitMs);
    }
    std::printf("%s", table.c_str());
    OX_LOG_INFO("test", "{}", table);
    renderWith("Off", "Auto", 1);
}
