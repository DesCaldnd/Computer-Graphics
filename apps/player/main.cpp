// OxwaldPlayer: runs a project (loose directory or cooked pak) without the editor.
#include <oxwald/core/log.hpp>
#include <oxwald/runtime/runtime.hpp>
#if OX_HAS_RENDER && OX_RENDER_HAS_RUNTIME
#include <oxwald/render/render_stats.hpp>
#include <oxwald/render/renderer.hpp>
#include <oxwald/render/runtime_renderer.hpp>

#include <cstring>
#include <map>
#endif
#if OX_HAS_UI
#include <oxwald/ui/ui_module.hpp>
#endif

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

// Averages of EngineStats + IRenderer::stats() over one-second windows (and the whole run, minus the first second).
class FrameStatsPrinter {
public:
    void add(const ox::EngineStats& e, const ox::RenderStats& r) {
        Sum& w = m_window;
        w.frames++;
        w.frameMs += e.frameMs;
        w.gameMs += e.gameMs;
        w.waitMs += e.waitForRenderMs;
        w.sleepMs += e.sleepMs;
        w.renderMs += r.lastRenderMs;
        w.slotWaitMs += r.lastFrameWaitMs;
        w.acquireMs += r.lastAcquireMs;
        w.cpuMs += r.lastCpuRenderMs;
        w.presentMs += r.lastPresentMs;
        w.gpuMs += r.lastGpuFrameMs;
        w.maxFrameMs = std::max(w.maxFrameMs, e.frameMs);
        w.latSleepMs += e.latencySleepMs;
        w.queueMs += e.waitForRenderMs + e.snapshotQueuedMs + r.latency.renderQueueWaitMs;
        w.engineLatencyMs += r.latency.engineLatencyMs;
        w.fgAddedMs += r.frameGeneration.addedLatencyMs;
        if (r.latency.reports > 0) {
            w.pcLatencyMs += r.latency.pcLatencyMs;
            w.pcLatencyFrames++;
        }
        m_vsync = r.vsync;
        m_latency = e.latency;
        m_fgActive = r.frameGeneration.active;
        m_fgRendered = r.frameGeneration.renderedFrames;
        m_fgPresented = r.frameGeneration.presentedFrames;
        if (w.frameMs >= 1000.0) {
            print("frame stats", w);
            if (m_windows++ > 0) m_total.merge(w); // the first second has loading and pipeline compiles
            w = {};
        }
    }
    void printTotal() {
        if (m_window.frames > 0 && m_windows > 0) m_total.merge(m_window);
        if (m_total.frames > 0) print("frame stats total", m_total);
    }

private:
    struct Sum {
        ox::u64 frames = 0;
        double frameMs = 0, gameMs = 0, waitMs = 0, sleepMs = 0, renderMs = 0, slotWaitMs = 0, acquireMs = 0, cpuMs = 0,
               presentMs = 0, gpuMs = 0, maxFrameMs = 0, latSleepMs = 0, queueMs = 0, pcLatencyMs = 0, engineLatencyMs = 0,
               fgAddedMs = 0;
        ox::u64 pcLatencyFrames = 0;
        void merge(const Sum& o) {
            frames += o.frames;
            latSleepMs += o.latSleepMs, queueMs += o.queueMs, pcLatencyMs += o.pcLatencyMs;
            engineLatencyMs += o.engineLatencyMs, fgAddedMs += o.fgAddedMs;
            pcLatencyFrames += o.pcLatencyFrames;
            frameMs += o.frameMs, gameMs += o.gameMs, waitMs += o.waitMs, sleepMs += o.sleepMs, renderMs += o.renderMs;
            slotWaitMs += o.slotWaitMs, acquireMs += o.acquireMs, cpuMs += o.cpuMs, presentMs += o.presentMs;
            gpuMs += o.gpuMs;
            maxFrameMs = std::max(maxFrameMs, o.maxFrameMs);
        }
    };
    void print(const char* label, const Sum& s) const {
        const double n = double(s.frames);
        std::printf("OxwaldPlayer: %s: fps=%.1f frame=%.2fms (max %.2f) | game=%.2f waitRender=%.2f sleep=%.2f | "
                    "render=%.2f [gpuSlotWait=%.2f acquire=%.2f cpu=%.2f present=%.2f] | gpu=%.2fms vsync=%d\n",
                    label, s.frameMs > 0 ? 1000.0 * n / s.frameMs : 0.0, s.frameMs / n, s.maxFrameMs, s.gameMs / n,
                    s.waitMs / n, s.sleepMs / n, s.renderMs / n, s.slotWaitMs / n, s.acquireMs / n, s.cpuMs / n,
                    s.presentMs / n, s.gpuMs / n, m_vsync ? 1 : 0);
        // r.Reflex: mode, who paced (driver / engine limiter), latency sleep, queueing, driver PC latency estimate.
        // engineLatency: simulation start -> GPU done measured by the engine (any vendor); pcLatency: the driver's
        // Reflex report of the same span (NVIDIA only; n/a through the frame generation proxy).
        std::printf("OxwaldPlayer: %s: latency mode=%d reflex=%s limiter=%d sleep=%.2fms queue=%.2fms "
                    "engineLatency=%.2fms pcLatency=%s\n",
                    label, m_latency.mode,
                    m_latency.reflexSupported               ? (m_latency.reflexActive ? "active" : "supported")
                    : m_latency.reflexBehindFrameGeneration ? "framegen-proxy"
                                                            : "unsupported",
                    m_latency.limiterActive ? 1 : 0, s.latSleepMs / n, s.queueMs / n, s.engineLatencyMs / n,
                    s.pcLatencyFrames ? std::to_string(s.pcLatencyMs / double(s.pcLatencyFrames)).substr(0, 6).append("ms").c_str()
                                      : "n/a");
        if (m_fgActive && m_fgRendered > 0) {
            const double ratio = double(m_fgPresented) / double(m_fgRendered);
            std::printf("OxwaldPlayer: %s: frame generation (FSR 3) presented=%llu rendered=%llu ratio=%.2f output fps=%.1f "
                        "addedLatency=%.2fms\n",
                        label, (unsigned long long)m_fgPresented, (unsigned long long)m_fgRendered, ratio,
                        s.frameMs > 0 ? 1000.0 * n / s.frameMs * ratio : 0.0, s.fgAddedMs / n);
        }
        std::fflush(stdout);
    }
    Sum m_window, m_total;
    int m_windows = 0;
    bool m_vsync = false;
    ox::LatencyStats m_latency;
    bool m_fgActive = false;
    ox::u64 m_fgRendered = 0, m_fgPresented = 0;

};

#if OX_HAS_RENDER && OX_RENDER_HAS_RUNTIME
// --pass-stats: GPU time of every render graph pass, averaged over the run after a warm-up of 60 frames, plus the
// distribution of the GPU frame time (bimodal frame times show up as a wide p10..p90). --pass-csv <file>: one row per
// retired frame and pass. Reads render::Renderer::stats() on the game thread, so it needs --single-thread.
class PassStatsPrinter {
public:
    void add(const ox::render::RenderStats& st, FILE* csv) {
        if (st.frame == m_lastFrame || st.gpuFrameMs <= 0.0) return;
        m_lastFrame = st.frame;
        if (++m_seen <= 60) return;
        m_frameMs.push_back(st.gpuFrameMs);
        std::map<std::string, double> perPass;
        for (const ox::render::PassTiming& p : st.passes) perPass[p.name.substr(p.name.find('/') + 1)] += p.gpuMs;
        for (const auto& [name, ms] : perPass) {
            Pass& s = m_passes[name];
            s.sum += ms;
            s.max = std::max(s.max, ms);
            s.frames++;
            if (csv) std::fprintf(csv, "%llu,%.4f,%s,%.4f\n", (unsigned long long)st.frame, st.gpuFrameMs, name.c_str(), ms);
        }
    }
    void print(std::size_t top) const {
        if (m_frameMs.empty()) {
            std::printf("OxwaldPlayer: pass stats: no GPU timings (fewer than 61 frames or no timestamp queries)\n");
            return;
        }
        std::vector<double> f = m_frameMs;
        std::sort(f.begin(), f.end());
        auto pct = [&](double q) { return f[std::min(f.size() - 1, std::size_t(q * double(f.size() - 1) + 0.5))]; };
        double mean = 0.0;
        for (double v : f) mean += v / double(f.size());
        std::printf("OxwaldPlayer: pass stats: %zu frames, GPU frame mean %.2f ms, min %.2f p10 %.2f median %.2f p90 %.2f max %.2f\n",
                    f.size(), mean, f.front(), pct(0.1), pct(0.5), pct(0.9), f.back());
        std::vector<std::pair<std::string, Pass>> rows(m_passes.begin(), m_passes.end());
        const double n = double(m_frameMs.size());
        std::sort(rows.begin(), rows.end(), [&](const auto& a, const auto& b) { return a.second.sum > b.second.sum; });
        for (std::size_t i = 0; i < rows.size() && i < top; ++i) {
            const auto& [name, s] = rows[i];
            std::printf("OxwaldPlayer:   %-40s avg %7.3f ms  max %7.3f ms  (in %llu/%zu frames)\n", name.c_str(), s.sum / n,
                        s.max, (unsigned long long)s.frames, m_frameMs.size());
        }
        std::fflush(stdout);
    }

private:
    struct Pass {
        double sum = 0.0, max = 0.0;
        ox::u64 frames = 0;
    };
    ox::u64 m_lastFrame = ~0ull, m_seen = 0;
    std::vector<double> m_frameMs;
    std::map<std::string, Pass> m_passes;
};
#endif


} // namespace

int main(int argc, char** argv) {
    // Player-only option: --screenshot <file.png> (written after the last of --frames N frames, windowed or headless).
    std::vector<std::string> args;
    std::string screenshot;
    // --stats (or OX_FRAME_STATS=1): print frame timing averages every second and at exit.
    bool printStats = std::getenv("OX_FRAME_STATS") && *std::getenv("OX_FRAME_STATS") && *std::getenv("OX_FRAME_STATS") != '0';
    // --pass-stats / --pass-csv <file>: per render graph pass GPU times (see PassStatsPrinter; forces --single-thread).
    bool passStats = false;
    std::string passCsv;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--") {
            for (; i < argc; ++i) args.emplace_back(argv[i]);
            break;
        }
        if (a == "--stats") {
            printStats = true;
            continue;
        }
        if (a == "--screenshot" && i + 1 < argc) {
            screenshot = argv[++i];
            continue;
        }
        if (a == "--pass-stats") {
            passStats = true;
            continue;
        }
        if (a == "--pass-csv" && i + 1 < argc) {
            passStats = true;
            passCsv = argv[++i];
            continue;
        }
        args.push_back(a);
    }
    auto options = ox::parseLaunchOptions(args);
    if (!options) {
        std::fprintf(stderr, "OxwaldPlayer: %s\n%s", options.error().message.c_str(), ox::launchUsage().c_str());
        return 2;
    }
    if (options->help) {
        std::printf("%s                    [--screenshot file.png] (PNG of the last of --frames N frames)\n"
                    "                    [--stats] [--pass-stats] [--pass-csv file.csv] (GPU time per render pass; "
                    "single-threaded)\n",
                    ox::launchUsage().c_str());
        return 0;
    }

    ox::Engine engine;
    ox::EngineConfig config = options->toEngineConfig("OxwaldPlayer");

#if OX_HAS_GLFW
    std::unique_ptr<ox::GlfwPlatform> platform;
    if (!config.headless) {
        ox::WindowDesc window;
        window.title = "Oxwald Player";
        if (options->width) window.size.x = *options->width;
        if (options->height) window.size.y = *options->height;
        if (options->windowMode) window.mode = *options->windowMode;
        if (options->monitor) window.monitor = *options->monitor;
        auto p = ox::GlfwPlatform::create(window, nullptr);
        if (!p) {
            OX_LOG_WARN("player", "{} — running headless", p.error().message);
            config.headless = true;
        } else {
            platform = std::move(*p);
            engine.setPlatform(platform.get());
        }
    }
#else
    if (!config.headless) {
        OX_LOG_WARN("player", "built without GLFW — running headless");
        config.headless = true;
    }
#endif
#if OX_HAS_RENDER && OX_RENDER_HAS_RUNTIME
    // Vulkan renderer (headless runs render offscreen). Dedicated servers keep the NullRenderer.
    ox::render::RuntimeRendererOptions rendererOptions;
    if (options->width) rendererOptions.headlessWidth = static_cast<ox::u32>(*options->width); // offscreen target size
    if (options->height) rendererOptions.headlessHeight = static_cast<ox::u32>(*options->height);
    ox::IRenderer* vulkanRenderer = nullptr; // the runtime renderer itself (withUi wraps it), for --pass-stats
    if (passStats) config.threadedRendering = false; // render::Renderer::stats() is read on the game thread
#if OX_HAS_UI
    // Debug overlay (F1 / ~) and RmlUi game UI on top of the renderer.
    if (!config.dedicatedServer) {
        engine.addModule(std::make_unique<ox::ui::UiModule>());
        std::unique_ptr<ox::IRenderer> inner = ox::render::createRenderer(rendererOptions);
        vulkanRenderer = inner.get();
        engine.setRenderer(ox::ui::withUi(std::move(inner)));
    }
#else
    if (!config.dedicatedServer) {
        std::unique_ptr<ox::IRenderer> inner = ox::render::createRenderer(rendererOptions);
        vulkanRenderer = inner.get();
        engine.setRenderer(std::move(inner));
    }
#endif
#endif

    if (auto st = engine.init(config); !st) {
        std::fprintf(stderr, "OxwaldPlayer: %s\n", st.error().message.c_str());
        return 1;
    }
#if OX_HAS_GLFW
    if (platform) {
        platform->setInput(&engine.input());
        platform->setTitle(engine.projectSettings().name);
        engine.applyGraphicsSettings();
    }
#endif
    FrameStatsPrinter statsPrinter;
    ox::ScopedConnection statsConnection;
    if (printStats) {
        statsConnection = engine.frameEnded.connect([&](const ox::EngineStats& es) { statsPrinter.add(es, engine.renderer().stats()); });
    }
#if OX_HAS_RENDER && OX_RENDER_HAS_RUNTIME
    PassStatsPrinter passPrinter;
    ox::ScopedConnection passConnection;
    FILE* passCsvFile = nullptr;
    ox::render::Renderer* passRenderer = vulkanRenderer ? ox::render::rendererOf(*vulkanRenderer) : nullptr;
    if (passStats && passRenderer) {
        if (!passCsv.empty()) {
            passCsvFile = std::fopen(passCsv.c_str(), "w");
            if (passCsvFile) std::fprintf(passCsvFile, "frame,gpu_frame_ms,pass,gpu_ms\n");
        }
        passConnection = engine.frameEnded.connect([&](const ox::EngineStats&) { passPrinter.add(passRenderer->stats(), passCsvFile); });
    } else if (passStats) {
        std::fprintf(stderr, "OxwaldPlayer: --pass-stats needs the Vulkan renderer\n");
    }
#endif
    std::atomic<int> screenshotResult{-1};
    if (!screenshot.empty() && options->frames > 0) {
        if (options->frames > 1) engine.run(options->frames - 1); // run(0) would not stop
        if (!engine.quitRequested() &&
            engine.renderer().requestScreenshot(screenshot, [&](bool ok) { screenshotResult.store(ok ? 1 : 0); })) {
            engine.run(1);
        } else if (!engine.quitRequested()) {
            std::fprintf(stderr, "OxwaldPlayer: --screenshot needs the Vulkan renderer\n");
        }
    } else {
        engine.run(options->frames);
    }
    if (printStats) statsPrinter.printTotal();
#if OX_HAS_RENDER && OX_RENDER_HAS_RUNTIME
    if (passStats && passRenderer) passPrinter.print(40);
    if (passCsvFile) std::fclose(passCsvFile);
#endif
    const ox::EngineStats stats = engine.stats();
    std::printf("OxwaldPlayer: frames=%llu fixedSteps=%llu fps=%.1f renderer=%s\n",
                static_cast<unsigned long long>(stats.frame + 1), static_cast<unsigned long long>(stats.totalFixedSteps),
                stats.fps, std::string(engine.renderer().name()).c_str());
    engine.shutdown(); // joins the render thread: a requested screenshot has been written now
#if OX_HAS_GLFW
    platform.reset();
#endif
    if (!screenshot.empty()) {
        std::printf("OxwaldPlayer: screenshot %s %s\n", screenshot.c_str(),
                    screenshotResult.load() == 1 ? "written" : "FAILED");
        if (screenshotResult.load() != 1) return 3;
    }
    return 0;
}
