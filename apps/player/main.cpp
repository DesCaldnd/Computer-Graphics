// OxwaldPlayer: runs a project (loose directory or cooked pak) without the editor.
#include <oxwald/core/log.hpp>
#include <oxwald/runtime/runtime.hpp>

#include <cstdio>

int main(int argc, char** argv) {
    auto options = ox::parseLaunchOptions(argc, argv);
    if (!options) {
        std::fprintf(stderr, "OxwaldPlayer: %s\n%s", options.error().message.c_str(), ox::launchUsage().c_str());
        return 2;
    }
    if (options->help) {
        std::printf("%s", ox::launchUsage().c_str());
        return 0;
    }
    if (!options->pak.empty()) {
        // TODO(assets): mount the pak over project:// once the assets module exposes its IMountSource.
        OX_LOG_WARN("player", "--pak is not supported yet; use --project with a loose project directory");
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
    // TODO(render): engine.setRenderer(ox::render::createRenderer()) once the render module implements IRenderer.
    // Until then the NullRenderer keeps the frame pipeline (game thread + render thread) running.

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
    engine.run(options->frames);
    const ox::EngineStats stats = engine.stats();
    std::printf("OxwaldPlayer: frames=%llu fixedSteps=%llu fps=%.1f renderer=%s\n",
                static_cast<unsigned long long>(stats.frame + 1), static_cast<unsigned long long>(stats.totalFixedSteps),
                stats.fps, std::string(engine.renderer().name()).c_str());
    engine.shutdown();
#if OX_HAS_GLFW
    platform.reset();
#endif
    return 0;
}
