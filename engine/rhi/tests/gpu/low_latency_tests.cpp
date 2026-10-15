// NVIDIA Reflex through VK_NV_low_latency2 on a real (small, windowed) swapchain: markers in frame order as reported
// back by vkGetLatencyTimingsNV, one vkLatencySleepNV per frame, mode toggles and swapchain recreation without
// validation errors or leaked objects. Skipped without a display or without the extension (non-NVIDIA, old driver).
#include "gpu/gpu_fixture.hpp"

#include <oxwald/rhi/swapchain.hpp>

#if OX_RHI_TESTS_HAVE_GLFW
#include <oxwald/rhi/glfw_surface.hpp>

#include <GLFW/glfw3.h>
#endif

#include <chrono>
#include <atomic>
#include <cstdio>
#include <thread>

namespace ox::rhi::test {

#if OX_RHI_TESTS_HAVE_GLFW

namespace {

class LowLatencyTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!initGlfwVulkan() || !glfwInit()) GTEST_SKIP() << "GLFW/Vulkan loader unavailable (no display?)";
        m_glfw = true;
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
        m_window = glfwCreateWindow(320, 240, "ox_rhi_gpu_tests low latency", nullptr, nullptr);
        if (!m_window) GTEST_SKIP() << "cannot create a window";
        m_surface = std::make_unique<GlfwSurfaceProvider>(m_window);
        Device::resetValidationCounters();
        DeviceDesc desc;
        desc.appName = "ox_rhi_gpu_tests";
        desc.validation = true;
        desc.surface = m_surface.get();
        desc.enableRayTracing = false;
        std::string error;
        device = Device::create(desc, &error);
        if (!device) GTEST_SKIP() << "no Vulkan device: " << error;
        if (!device->caps().lowLatency2) GTEST_SKIP() << "VK_NV_low_latency2 not available (" << device->caps().gpuName << ")";
        swapchain = Swapchain::create(*device, {PresentMode::Mailbox});
        ASSERT_NE(swapchain, nullptr);
        ASSERT_TRUE(swapchain->lowLatencySupported());
    }

    void TearDown() override {
        if (device) {
            device->waitIdle();
            swapchain.reset();
            device.reset();
            EXPECT_EQ(Device::validationErrorCount(), 0u) << "Vulkan validation reported errors (see log)";
        }
        m_surface.reset();
        if (m_window) glfwDestroyWindow(m_window);
        if (m_glfw) glfwTerminate();
    }

    // One frame the way the runtime does it: sleep, simulation + input markers (game thread), clear + submit with
    // render submit markers (render thread), present (present markers inside Swapchain::present).
    bool frame(u64 presentId, bool sleep = true) {
        glfwPollEvents();
        if (sleep) swapchain->latencySleep();
        swapchain->setLatencyMarker(presentId, LatencyMarker::SimulationStart);
        swapchain->setLatencyMarker(presentId, LatencyMarker::InputSample);
        swapchain->setLatencyMarker(presentId, LatencyMarker::SimulationEnd);
        device->beginFrame();
        if (!swapchain->acquire()) {
            device->endFrame();
            return false;
        }
        swapchain->setPresentId(presentId);
        swapchain->setLatencyMarker(presentId, LatencyMarker::RenderSubmitStart);
        CommandList& cmd = device->commandList(QueueType::Graphics, "lowLatency.clear");
        const TextureHandle image = swapchain->currentTexture();
        cmd.transition(image, Access::TransferWrite, true);
        cmd.clearTexture(image, ClearColor::rgba(0.1f, f32(presentId % 60) / 60.0f, 0.3f));
        cmd.transition(image, Access::Present);
        SubmitInfo si;
        si.swapchain = swapchain.get();
        device->submit(cmd, si);
        swapchain->setLatencyMarker(presentId, LatencyMarker::RenderSubmitEnd);
        swapchain->present();
        device->endFrame();
        return true;
    }

    std::unique_ptr<Device> device;
    std::unique_ptr<Swapchain> swapchain;

private:
    bool m_glfw = false;
    GLFWwindow* m_window = nullptr;
    std::unique_ptr<GlfwSurfaceProvider> m_surface;
};

} // namespace

TEST_F(LowLatencyTest, MarkersReportedInFrameOrderAndOneSleepPerFrame) {
    swapchain->setLowLatencyMode({.enabled = true, .boost = false});
    constexpr u64 kFrames = 90;
    u64 presented = 0;
    for (u64 i = 1; i <= kFrames; ++i) presented += frame(i) ? 1 : 0;
    device->waitIdle();
    ASSERT_GT(presented, kFrames / 2) << "window minimised / occluded?";
    EXPECT_EQ(swapchain->latencySleepCount(), kFrames) << "exactly one vkLatencySleepNV per frame";

    const std::vector<LatencyFrameTimings> t = swapchain->latencyTimings();
    ASSERT_FALSE(t.empty()) << "vkGetLatencyTimingsNV returned no frame reports";
    u32 complete = 0;
    u64 lastId = 0;
    for (const LatencyFrameTimings& f : t) {
        EXPECT_GT(f.presentId, lastId) << "reports sorted by present id";
        EXPECT_LE(f.presentId, kFrames);
        lastId = f.presentId;
        if (!f.complete() || !f.presentEndUs || !f.inputSampleUs) continue;
        ++complete;
        // The order the markers were set in, as the driver timestamped them.
        EXPECT_LE(f.simStartUs, f.inputSampleUs) << "frame " << f.presentId;
        EXPECT_LE(f.inputSampleUs, f.simEndUs) << "frame " << f.presentId;
        EXPECT_LE(f.simEndUs, f.renderSubmitStartUs) << "frame " << f.presentId;
        EXPECT_LE(f.renderSubmitStartUs, f.renderSubmitEndUs) << "frame " << f.presentId;
        EXPECT_LE(f.renderSubmitEndUs, f.presentStartUs) << "frame " << f.presentId;
        EXPECT_LE(f.presentStartUs, f.presentEndUs) << "frame " << f.presentId;
        EXPECT_GT(f.gpuRenderEndUs, 0u) << "GPU timing tied to the frame (VkLatencySubmissionPresentIdNV)";
        EXPECT_LT(f.pcLatencyMs(), 1000.0);
    }
    EXPECT_GT(complete, 0u);
    const LatencyFrameTimings& last = t.back();
    std::printf("[ reflex ] %zu reports, last frame %llu: PC latency %.3f ms, sim->submit %.3f ms, queue %.3f ms, GPU %.3f ms\n",
                t.size(), (unsigned long long)last.presentId, last.pcLatencyMs(), last.gameToRenderMs(), last.renderQueueMs(),
                last.gpuRenderMs());
}

TEST_F(LowLatencyTest, TogglingModesAndRecreatingTheSwapchainIsClean) {
    u64 id = 1;
    const LowLatencyMode modes[] = {{true, false, 0}, {true, true, 0}, {false, false, 0}, {true, false, 4000}, {false, false, 0}};
    for (int round = 0; round < 2; ++round) {
        for (const LowLatencyMode& m : modes) {
            swapchain->setLowLatencyMode(m);
            EXPECT_EQ(swapchain->lowLatencyMode().enabled, m.enabled);
            for (int i = 0; i < 6; ++i) frame(id++, m.enabled);
        }
        // Present mode change -> new VkSwapchainKHR on the next acquire; the latency mode is re-applied to it.
        swapchain->setPresentMode(round == 0 ? PresentMode::VSync : PresentMode::Mailbox);
        swapchain->setLowLatencyMode({true, true, 0});
        for (int i = 0; i < 6; ++i) frame(id++);
        EXPECT_TRUE(swapchain->lowLatencyMode().boost);
    }
    // Sleeping with the mode off is a no-op that reports "not slept".
    swapchain->setLowLatencyMode({});
    EXPECT_LT(swapchain->latencySleep(), 0.0);
}

TEST_F(LowLatencyTest, ConcurrentGameThreadSleepWhileTheRenderThreadRecreates) {
    swapchain->setLowLatencyMode({true, false, 0});
    std::atomic<bool> stop{false};
    std::atomic<u64> sleeps{0};
    std::thread game([&] {
        u64 id = 1000;
        while (!stop.load()) {
            swapchain->latencySleep(20);
            swapchain->setLatencyMarker(++id, LatencyMarker::SimulationStart);
            sleeps.fetch_add(1);
        }
    });
    u64 id = 1;
    for (int i = 0; i < 40; ++i) {
        if (i % 10 == 5) swapchain->setPresentMode(i % 20 == 5 ? PresentMode::VSync : PresentMode::Mailbox);
        frame(id++, false);
    }
    stop.store(true);
    game.join();
    EXPECT_GT(sleeps.load(), 0u);
}

#else

TEST(LowLatencyTest, RequiresGlfw) { GTEST_SKIP() << "built without GLFW"; }

#endif

} // namespace ox::rhi::test
