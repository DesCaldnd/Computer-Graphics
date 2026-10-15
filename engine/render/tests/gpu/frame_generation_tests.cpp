// FSR 3 frame generation (render/features/framegen) on a real window: the proxy presents one generated frame per
// rendered frame, a generated frame of a panning camera matches the rendered intermediate frame (PSNR), toggling
// on/off (swapchain in and out of the proxy) leaves no validation errors or leaked objects. Skipped without a display
// or without amd_fidelityfx_vk.dll.
#include "render_fixture.hpp"

#include <oxwald/render/features/framegen/frame_generation.hpp>
#include <oxwald/rhi/swapchain.hpp>

#if OX_RENDER_TESTS_HAVE_GLFW
#include <oxwald/rhi/glfw_surface.hpp>

#include <GLFW/glfw3.h>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace ox::render::test {

#if OX_RENDER_TESTS_HAVE_GLFW

namespace {

constexpr u32 kWidth = 640, kHeight = 360;

// The FSR 3 proxy runs its own pacing / present threads and blocking waits inside the driver: a regression there
// hangs the process instead of failing an expectation. The watchdog turns a hang into a failure with the phase it was
// stuck in (ctest would otherwise only report a timeout after 10 minutes).
class Watchdog {
public:
    explicit Watchdog(std::chrono::seconds limit) : m_limit(limit), m_thread([this] { run(); }) {}
    ~Watchdog() {
        {
            std::lock_guard lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
        m_thread.join();
    }
    std::function<void()> diag; // extra state printed on a hang (set once, before frames run)
    // Restarts the countdown for a new phase.
    void phase(std::string name) {
        std::lock_guard lock(m_mutex);
        m_phase = std::move(name);
        m_deadline = std::chrono::steady_clock::now() + m_limit;
        m_cv.notify_all();
    }

private:
    void run() {
        std::unique_lock lock(m_mutex);
        while (!m_stop) {
            if (m_cv.wait_until(lock, m_deadline, [&] { return m_stop; })) return;
            if (std::chrono::steady_clock::now() < m_deadline) continue; // phase() moved the deadline
            std::fprintf(stderr, "\n[  FAILED  ] watchdog: '%s' made no progress for %lld s (frame generation proxy hang)\n",
                         m_phase.c_str(), static_cast<long long>(m_limit.count()));
            if (diag) diag();
            std::fflush(stderr);
            std::fflush(stdout);
            std::_Exit(3);
        }
    }

    std::chrono::seconds m_limit;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stop = false;
    std::string m_phase = "set up";
    std::chrono::steady_clock::time_point m_deadline = std::chrono::steady_clock::now() + m_limit;
    std::thread m_thread;
};

// Machine-wide lock (named Win32 mutex; no-op elsewhere — frame generation is Windows-only). An abandoned mutex (the
// previous holder hit the watchdog) is still acquired.
class ProcessLock {
public:
    void acquire() {
#if defined(_WIN32)
        if (!m_handle) m_handle = CreateMutexW(nullptr, FALSE, L"Local\\OxwaldFrameGenerationGpuTest");
        if (m_handle) {
            const DWORD r = WaitForSingleObject(m_handle, 10 * 60 * 1000);
            m_held = r == WAIT_OBJECT_0 || r == WAIT_ABANDONED;
        }
#endif
    }
    void release() {
#if defined(_WIN32)
        if (m_held) ReleaseMutex(m_handle);
        m_held = false;
#endif
    }
    ~ProcessLock() {
        release();
#if defined(_WIN32)
        if (m_handle) CloseHandle(m_handle);
#endif
    }

private:
#if defined(_WIN32)
    HANDLE m_handle = nullptr;
#endif
    bool m_held = false;
};

class FrameGenerationTest : public RenderTest {
protected:
    void SetUp() override {
        // One frame generation test process at a time (ctest -j runs each test in its own process): two FSR 3
        // swapchain proxies presenting at once on one GPU deadlock inside FidelityFX SDK 1.1.4 / the driver within a
        // few frames (measured: 9 of 9 runs with 3 concurrent processes; 0 of 11 next to other GPU suites or other
        // windowed presenters). See docs/dev/modules/render_postprocess.md "Frame generation".
        m_processLock.acquire();
        m_watchdog = std::make_unique<Watchdog>(std::chrono::seconds(60));
        registerSceneTypes();
        if (!rhi::initGlfwVulkan() || !glfwInit()) GTEST_SKIP() << "GLFW/Vulkan loader unavailable (no display?)";
        m_glfw = true;
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
        m_window = glfwCreateWindow(int(kWidth), int(kHeight), "ox_render_gpu_tests frame generation", nullptr, nullptr);
        if (!m_window) GTEST_SKIP() << "cannot create a window";
        m_surface = std::make_unique<rhi::GlfwSurfaceProvider>(m_window);
        rhi::Device::resetValidationCounters();
        rhi::DeviceDesc desc;
        desc.appName = "ox_render_gpu_tests";
        desc.validation = true;
        desc.surface = m_surface.get();
        framegen::appendVulkanRequirements(desc);
        std::string error;
        device = rhi::Device::create(desc, &error);
        if (!device) GTEST_SKIP() << "no Vulkan device: " << error;
        renderer = Renderer::create(*device);
        world = std::make_unique<World>();
        swapchain = rhi::Swapchain::create(*device, {rhi::PresentMode::Mailbox});
        ASSERT_NE(swapchain, nullptr);
        fgView = renderer->createView({.name = "Main"});
        std::string reason;
        frameGen = framegen::FrameGenerator::create(*device, *renderer, fgView, &reason);
        if (!frameGen) GTEST_SKIP() << "frame generation unavailable: " << reason;
        // On a hang: is our own GPU work done (graphics timeline vs submitted) and how far the proxy's present thread got.
        m_watchdog->diag = [this] {
            if (!m_diagSafe.load()) return;
            u64 v = 0;
            vkGetSemaphoreCounterValue(device->vkDevice(), device->timelineSemaphore(rhi::QueueType::Graphics), &v);
            const framegen::Stats st = frameGen->stats();
            std::fprintf(stderr, "[ framegen ] graphics timeline %llu / submitted %llu; fg frame %llu composed gen %llu real %llu timeouts %llu\n",
                         (unsigned long long)v, (unsigned long long)device->lastSubmitted(rhi::QueueType::Graphics).value,
                         (unsigned long long)frameGen->currentFrameId(), (unsigned long long)st.generatedPresented,
                         (unsigned long long)st.realPresented, (unsigned long long)st.pacingTimeouts);
        };
    }

    void TearDown() override {
        m_diagSafe.store(false);
        if (m_watchdog) m_watchdog->phase("tear down");
        if (device) {
            device->waitIdle();
            renderer.reset();     // owns the frame generation feature
            swapchain.reset();    // destroys the proxy swapchain through the generator
            frameGen.reset();
        }
        RenderTest::TearDown();
        m_surface.reset();
        if (m_window) glfwDestroyWindow(m_window);
        if (m_glfw) glfwTerminate();
        m_watchdog.reset();
        m_processLock.release();
    }

    void buildScene() {
        environment(1.0f, 1.0f);
        sun(glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f)), 20000.0f);
        const Uuid ground = material({0.6f, 0.6f, 0.6f, 1}, 0, 0.8f);
        mesh(Primitive::Plane, ground, {0, 0, 0}, {40, 1, 40});
        for (int i = 0; i < 9; ++i) {
            const Uuid m = material({0.2f + 0.08f * f32(i), 0.3f, 0.9f - 0.08f * f32(i), 1}, 0, 0.5f);
            mesh(Primitive::Cube, m, {-8.0f + 2.0f * f32(i), 0.75f, -6.0f - 1.5f * f32(i % 3)}, glm::vec3(1.2f));
        }
    }

    // Camera panning along +x: one step per rendered frame.
    static CameraParams panCamera(f64 step) {
        const f32 x = f32(-2.0 + 0.12 * step);
        return camera({x, 2.0f, 6.0f}, {x, 1.0f, -6.0f}, 13.0f, 60.0f, 200.0f);
    }

    // One runtime-style frame through the swapchain (and the proxy when enabled).
    bool frame(const CameraParams& cam, bool enable) {
        m_watchdog->phase(std::format("frame {} (frame generation {})", m_frames++, enable ? "on" : "off"));
        glfwPollEvents();
        frameGen->beginFrame(*swapchain, enable);
        device->beginFrame();
        if (!swapchain->acquire()) {
            device->endFrame();
            return false;
        }
        frameGen->prepareFrame(*swapchain, 16.6f);
        renderer->beginFrame(snapshot);
        ViewRenderRequest req;
        req.view = fgView;
        req.camera = cam;
        req.target.swapchain = swapchain.get();
        renderer->renderView(req);
        renderer->endFrame();
        swapchain->present();
        device->endFrame();
        frameGen->endFrame(*swapchain);
        return true;
    }

    std::unique_ptr<rhi::Swapchain> swapchain;
    std::unique_ptr<framegen::FrameGenerator> frameGen;
    ViewId fgView = 0;
    std::unique_ptr<Watchdog> m_watchdog;
    std::atomic<bool> m_diagSafe{true};

private:
    u64 m_frames = 0;
    bool m_glfw = false;
    GLFWwindow* m_window = nullptr;
    std::unique_ptr<rhi::GlfwSurfaceProvider> m_surface;
    ProcessLock m_processLock;
};

// PSNR over RGB of a BGRA/RGBA capture against an RGBA image.
f64 psnr(const std::vector<u8>& capture, bool bgra, const Image& ref) {
    f64 mse = 0.0;
    const usize n = usize(ref.width) * ref.height;
    for (usize i = 0; i < n; ++i) {
        const u8* c = &capture[i * 4];
        const u8 rgb[3] = {bgra ? c[2] : c[0], c[1], bgra ? c[0] : c[2]};
        for (int k = 0; k < 3; ++k) {
            const f64 d = f64(rgb[k]) - f64(ref.rgba[i * 4 + usize(k)]);
            mse += d * d;
        }
    }
    mse /= f64(n) * 3.0;
    return mse <= 1e-9 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

} // namespace

TEST_F(FrameGenerationTest, PresentsOneGeneratedFramePerRenderedFrame) {
    buildScene();
    extractSnapshot();
    renderer->resources().flush();
    constexpr int kFrames = 150;
    for (int i = 0; i < kFrames; ++i) frame(panCamera(f64(i)), true);
    m_watchdog->phase("waitIdle");
    device->waitIdle();
    const framegen::Stats s = frameGen->stats();
    ASSERT_TRUE(s.active) << s.status;
    ASSERT_GT(s.renderedFrames, u64(kFrames) / 2);
    const f64 ratio = f64(s.presentedFrames) / f64(s.renderedFrames);
    std::printf("[ framegen ] rendered %llu, presented %llu (%.2fx): %llu generated + %llu rendered composed\n",
                (unsigned long long)s.renderedFrames, (unsigned long long)s.presentedFrames, ratio,
                (unsigned long long)s.generatedPresented, (unsigned long long)s.realPresented);
    // Every frame the proxy shows comes in a pair: one generated frame, then the rendered one.
    ASSERT_GT(s.realPresented, u64(kFrames) / 2);
    EXPECT_NEAR(f64(s.generatedPresented) / f64(s.realPresented), 1.0, 0.03);
    // Total output: 2x minus the pairs the proxy's pacer dropped. It overwrites a scheduled pair that its present thread
    // has not picked up yet (FidelityFX SDK 1.1.4 composeAndPresent_presenterThread: "we might have dropped entries"),
    // which happens when the machine is loaded (measured 1.80x-1.89x in 3 of 12 runs next to other jobs, 1.97x-1.99x on
    // an idle machine). Bound at 1.6x: a broken interpolation path (no generated frames) gives 1.0x.
    EXPECT_GT(ratio, 1.6);
    EXPECT_LE(ratio, 2.02);
}

TEST_F(FrameGenerationTest, GeneratedFrameMatchesTheRenderedIntermediateFrame) {
    CVarScope aa("r.AntiAliasing", "0"); // no TAA jitter / history: the reference is a single clean frame
    CVarScope up("r.Upscaler", "Off");
    buildScene();
    extractSnapshot();
    renderer->resources().flush();
    int i = 0;
    for (; i < 60; ++i) frame(panCamera(f64(i)), true);
    frameGen->captureNextGeneratedFrame();
    for (int k = 0; k < 8; ++k, ++i) {
        frame(panCamera(f64(i)), true);
        m_watchdog->phase("capturedFrame");
        if (frameGen->capturedFrame().done) break;
    }
    const framegen::FrameGenerator::CapturedFrame cap = frameGen->capturedFrame();
    m_watchdog->phase("reference renders");
    ASSERT_TRUE(cap.done) << "no generated frame was presented";
    ASSERT_EQ(cap.width, kWidth);
    ASSERT_EQ(cap.height, kHeight);
    // Frame id j is the frame rendered with panCamera(j - 1) (ids count from 1 since the proxy became active);
    // generated frame k sits between ids k - 1 and k.
    const f64 prevStep = f64(cap.frameId) - 2.0, curStep = f64(cap.frameId) - 1.0;
    Options o;
    o.width = kWidth;
    o.height = kHeight;
    o.frames = 3;
    const Image mid = render(panCamera(0.5 * (prevStep + curStep)), o);
    const Image prev = render(panCamera(prevStep), o);
    const Image cur = render(panCamera(curStep), o);
    const bool bgra = swapchain->format() == VK_FORMAT_B8G8R8A8_SRGB || swapchain->format() == VK_FORMAT_B8G8R8A8_UNORM;
    const f64 pMid = psnr(cap.pixels, bgra, mid), pPrev = psnr(cap.pixels, bgra, prev), pCur = psnr(cap.pixels, bgra, cur);
    std::printf("[ framegen ] generated frame %llu: PSNR vs intermediate %.2f dB, vs previous %.2f dB, vs current %.2f dB\n",
                (unsigned long long)cap.frameId, pMid, pPrev, pCur);
    Image out{kWidth, kHeight, cap.pixels};
    if (bgra) {
        for (usize p = 0; p < out.rgba.size(); p += 4) std::swap(out.rgba[p], out.rgba[p + 2]);
    }
    for (usize p = 3; p < out.rgba.size(); p += 4) out.rgba[p] = 255;
    writePng(std::filesystem::temp_directory_path() / "oxwald_render_out" / "framegen_generated.png", out);
    writePng(std::filesystem::temp_directory_path() / "oxwald_render_out" / "framegen_reference.png", mid);
    EXPECT_GT(pMid, 26.0);
    EXPECT_GT(pMid, pPrev + 1.0) << "the generated frame must be closer to the intermediate than to the rendered frames";
    EXPECT_GT(pMid, pCur + 1.0);
}

TEST_F(FrameGenerationTest, TogglingInAndOutOfTheProxyIsClean) {
    buildScene();
    extractSnapshot();
    renderer->resources().flush();
    int i = 0;
    for (int round = 0; round < 3; ++round) {
        for (int k = 0; k < 20; ++k, ++i) frame(panCamera(f64(i)), true);
        EXPECT_TRUE(frameGen->proxyActive());
        EXPECT_FALSE(swapchain->lowLatencySupported()) << "no Reflex calls on the proxy's swapchain";
        // Hidden, not lost (LatencyStats::reflexBehindFrameGeneration): the runtime reports why Reflex is off.
        EXPECT_EQ(swapchain->lowLatencyCapable(), device->caps().lowLatency2);
        EXPECT_LT(swapchain->latencySleep(), 0.0) << "latency sleep is a no-op through the proxy";
        for (int k = 0; k < 10; ++k, ++i) frame(panCamera(f64(i)), false);
        EXPECT_FALSE(frameGen->proxyActive());
        EXPECT_EQ(swapchain->activeProxy(), nullptr);
        EXPECT_EQ(swapchain->lowLatencySupported(), device->caps().lowLatency2) << "Reflex is back on the plain swapchain";
    }
    device->waitIdle();
    // TearDown checks the validation counters (including objects left at vkDestroyDevice).
}

#else

TEST(FrameGenerationTest, RequiresGlfw) { GTEST_SKIP() << "built without GLFW"; }

#endif

} // namespace ox::render::test
