#pragma once

#include <oxwald/core/result.hpp>
#include <oxwald/runtime/engine.hpp>

#include <optional>
#include <span>
#include <string>
#include <vector>

// Command line of OxwaldPlayer (and anything else that boots an Engine from argv).
//   --project <dir|file.oxproj>   project to run          --pak <file>        cooked package (assets module)
//   --scene <uri|path>            startup scene override   --headless          no window, NullRenderer
//   --server                      dedicated server         --frames <n>        quit after n frames
//   --width <px> --height <px>    window size              --fullscreen | --borderless | --windowed
//   --monitor <i>                 fullscreen monitor       --quality low|medium|high|ultra
//   --cvar name=value (repeat)    cvar override            --fps <n>           frame limit
//   --single-thread               no render thread         --user-dir <dir>    user:// root
//   --fixed-rate <hz>             FixedUpdate rate         --help
//   -- <args...>                  everything after "--" is passed to game code (EngineConfig::gameArgs)
//   --patch-pak <file> (repeat)   patch paks over --pak (oxpack --patch <base.oxpak>)
namespace ox {

struct LaunchOptions {
    std::filesystem::path project;
    std::filesystem::path pak;
    std::string scene;
    bool headless = false;
    bool server = false;
    u64 frames = 0;
    std::optional<i32> width;
    std::optional<i32> height;
    std::optional<WindowMode> windowMode;
    std::optional<i32> monitor;
    std::optional<QualityLevel> quality;
    std::vector<std::string> cvars;
    std::optional<f64> fps;
    std::optional<f64> fixedRate;
    bool singleThread = false;
    std::filesystem::path userDir;
    std::vector<std::filesystem::path> patchPaks; // --patch-pak (repeatable), mounted over --pak in order
    std::vector<std::string> gameArgs; // after "--"
    bool help = false;

    // Engine configuration for these options (project, scene, headless/server, threading, quality, cvars...).
    [[nodiscard]] EngineConfig toEngineConfig(std::string appName = "OxwaldPlayer") const;
};

[[nodiscard]] Result<LaunchOptions> parseLaunchOptions(std::span<const std::string> args);
[[nodiscard]] Result<LaunchOptions> parseLaunchOptions(int argc, const char* const* argv);
[[nodiscard]] std::string launchUsage();

} // namespace ox
