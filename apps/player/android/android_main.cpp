// OxwaldPlayer for Android: GameActivity entry point (liboxwald_player.so, loaded by OxwaldActivity).
//
// Launch arguments come from intent extras (tools/android/run.ps1 builds them):
//   --es args "<player command line>"   same options as the desktop player (--frames N, --scene, --cvar, --quality,
//                                       --stats, --screenshot <file.png>, --remote-console <port>, ...)
//   --ez wait_debugger true             block until lldb attached (tools/android/debug.ps1)
//   --es validation 1|0                 Vulkan validation (OX_VULKAN_VALIDATION; layers packaged in debug APKs)
// Without --project/--pak the cooked project packaged in the APK (assets/project/*.oxpak) runs.
#include <oxwald/android/android_app_utils.hpp>
#include <oxwald/android/android_assets.hpp>
#include <oxwald/android/android_log.hpp>
#include <oxwald/android/android_platform.hpp>
#include <oxwald/android/crash_handler.hpp>
#include <oxwald/android/perf_hints.hpp>
#include <oxwald/android/remote_console.hpp>
#include <oxwald/core/asset_bundle.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/core/vfs.hpp>
#include <oxwald/runtime/launch.hpp>
#include <oxwald/runtime/runtime.hpp>
#if OX_HAS_RENDER && OX_RENDER_HAS_RUNTIME
#include <oxwald/render/runtime_renderer.hpp>
#endif
#if OX_HAS_UI
#include <oxwald/ui/ui_module.hpp>
#endif

#include <game-activity/native_app_glue/android_native_app_glue.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#ifndef OX_ANDROID_DEFAULT_CONSOLE_PORT
#define OX_ANDROID_DEFAULT_CONSOLE_PORT 0
#endif

namespace {

namespace fs = std::filesystem;
using namespace ox;

struct PlayerArgs {
    std::vector<std::string> engineArgs;
    std::string screenshot;
    bool stats = false;
    int consolePort = OX_ANDROID_DEFAULT_CONSOLE_PORT;
};

PlayerArgs splitPlayerArgs(const std::vector<std::string>& all) {
    PlayerArgs p;
    for (size_t i = 0; i < all.size(); ++i) {
        const std::string& a = all[i];
        if (a == "--") {
            p.engineArgs.insert(p.engineArgs.end(), all.begin() + std::ptrdiff_t(i), all.end());
            break;
        }
        if (a == "--stats") {
            p.stats = true;
        } else if (a == "--screenshot" && i + 1 < all.size()) {
            p.screenshot = all[++i];
        } else if (a == "--remote-console" && i + 1 < all.size()) {
            p.consolePort = std::atoi(all[++i].c_str());
        } else if (a == "--no-remote-console") {
            p.consolePort = 0;
        } else {
            p.engineArgs.push_back(a);
        }
    }
    return p;
}

// Relative paths on the command line are relative to the app's internal storage (files dir).
fs::path resolve(const fs::path& p, const fs::path& base) { return p.empty() || p.is_absolute() ? p : base / p; }

int runPlayer(android_app* app, android::AndroidPlatform& platform) {
    const fs::path filesDir = app->activity->internalDataPath ? app->activity->internalDataPath : "/data/local/tmp";
    OX_LOG_INFO("player", "OxwaldPlayer {} (API {}, {}), files {}", android::abiName(), app->activity->sdkVersion,
#ifdef NDEBUG
                "release",
#else
                "debug",
#endif
                filesDir.string());

    const std::string argLine = android::intentStringExtra(app, "args").value_or("");
    if (auto v = android::intentStringExtra(app, "validation")) setenv("OX_VULKAN_VALIDATION", v->c_str(), 1);
    if (android::intentBoolExtra(app, "wait_debugger")) android::waitForDebugger(120.0);
    PlayerArgs player = splitPlayerArgs(splitCommandLine(argLine));
    if (!argLine.empty()) OX_LOG_INFO("player", "arguments: {}", argLine);

    auto bundle = std::make_shared<android::AndroidAssetBundle>(app->activity->assetManager);
    auto content = android::prepareContent(*bundle, filesDir);
    if (!content) {
        OX_LOG_ERROR("player", "APK content: {}", content.error().message);
        return 1;
    }

    auto options = parseLaunchOptions(player.engineArgs);
    if (!options) {
        OX_LOG_ERROR("player", "{}", options.error().message);
        return 2;
    }
    EngineConfig config = options->toEngineConfig("OxwaldPlayer");
    config.projectPath = resolve(config.projectPath, filesDir);
    config.pakPath = resolve(config.pakPath, filesDir);
    for (auto& p : config.patchPaks) p = resolve(p, filesDir);
    if (config.projectPath.empty() && config.pakPath.empty()) config.pakPath = content->pak;
    if (config.userDir.empty()) config.userDir = content->userDir;
    else config.userDir = resolve(config.userDir, filesDir);
    if (config.engineDir.empty()) config.engineDir = content->engineRoot;
    OX_LOG_INFO("player", "project {}, user dir {}", config.pakPath.empty() ? config.projectPath.string() : config.pakPath.string(),
                config.userDir.string());

    // The Vulkan device is created with the surface: wait for the first window.
    while (!platform.lifecycle().hasWindow() && !platform.shouldClose()) platform.pumpCommands(true);
    if (platform.shouldClose()) return 0;

    Engine engine;
    if (!config.headless) engine.setPlatform(&platform);
#if OX_HAS_RENDER && OX_RENDER_HAS_RUNTIME
    render::RuntimeRendererOptions rendererOptions;
    // First launch (no user://settings.json yet): render::mobileQualityProfile() picks Low/Medium + FSR 1 by GPU;
    // later launches keep the player's saved settings.
    rendererOptions.autoDetectQuality = true;
#if OX_HAS_UI
    if (!config.dedicatedServer) {
        engine.addModule(std::make_unique<ui::UiModule>());
        engine.setRenderer(ui::withUi(render::createRenderer(rendererOptions)));
    }
#else
    if (!config.dedicatedServer) engine.setRenderer(render::createRenderer(rendererOptions));
#endif
#endif
    if (auto st = engine.init(config); !st) {
        OX_LOG_ERROR("player", "engine init failed: {}", st.error().message);
        return 1;
    }
    // apk:// = the raw APK assets (read-only, AAssetManager).
    engine.vfs().mount("apk", std::make_unique<BundleMount>(bundle, "", content->index));
    platform.attach(&engine);
    engine.applyGraphicsSettings();

    android::RemoteConsoleServer console;
    if (player.consolePort > 0) console.start(u16(player.consolePort));
    android::PerformanceHints hints;
    hints.startSession({gettid()}, 16'666'667);

    std::atomic<u64> statsFrames{0};
    ScopedConnection frameConnection = engine.frameEnded.connect([&](const EngineStats& es) {
        hints.reportFrame(i64(es.gameMs * 1e6));
        const u64 n = statsFrames.fetch_add(1) + 1;
        if (player.stats && n % 60 == 0) {
            const RenderStats rs = engine.renderer().stats();
            std::printf("OxwaldPlayer: frame stats: fps=%.1f frame=%.2fms game=%.2f render=%.2f gpu=%.2fms\n", es.fps,
                        es.frameMs, es.gameMs, rs.lastRenderMs, rs.lastGpuFrameMs);
        }
        if (n % 120 == 0) hints.poll();
    });

    const u64 maxFrames = options->frames;
    std::atomic<int> screenshotResult{-1};
    bool screenshotRequested = false;
    const fs::path screenshotPath = resolve(player.screenshot, config.userDir);
    u64 frames = 0;
    while (!platform.shouldClose() && !engine.quitRequested()) {
        platform.pumpCommands(false);
        if (platform.shouldClose()) break;
        if (!platform.lifecycle().shouldRender()) {
            platform.pumpCommands(true); // sleeps in the looper until something happens
            continue;
        }
        console.pump(engine.console());
        if (!player.screenshot.empty() && maxFrames > 0 && frames + 1 == maxFrames && !screenshotRequested) {
            screenshotRequested = engine.renderer().requestScreenshot(
                screenshotPath, [&](bool ok) { screenshotResult.store(ok ? 1 : 0); });
        }
        if (!engine.tick()) break;
        if (maxFrames > 0 && ++frames >= maxFrames) break;
    }
    const EngineStats stats = engine.stats();
    std::printf("OxwaldPlayer: frames=%llu fixedSteps=%llu fps=%.1f renderer=%s\n",
                static_cast<unsigned long long>(stats.frame + 1), static_cast<unsigned long long>(stats.totalFixedSteps),
                stats.fps, std::string(engine.renderer().name()).c_str());
    frameConnection.disconnect();
    console.stop();
    platform.detach();
    engine.shutdown();
    if (!player.screenshot.empty()) {
        std::printf("OxwaldPlayer: screenshot %s %s\n", screenshotPath.c_str(), screenshotResult.load() == 1 ? "written" : "FAILED");
        if (screenshotResult.load() != 1) return 3;
    }
    return 0;
}

} // namespace

extern "C" void android_main(android_app* app) {
    android::installLogcatSink();
    android::redirectStdioToLogcat();
    android::installCrashHandler();
    int code = 0;
    {
        android::AndroidPlatform platform(app);
        code = runPlayer(app, platform);
        std::printf("OxwaldPlayer: exit code %d\n", code);
        std::fflush(stdout);
        // Close the activity and let the glue run APP_CMD_DESTROY (the process stays cached by the system).
        if (!app->destroyRequested) platform.finish();
        while (!app->destroyRequested) platform.pumpCommands(true);
    }
    OX_LOG_INFO("player", "android_main finished");
}
