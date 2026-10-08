#include <oxwald/core/scalability.hpp>
#include <oxwald/runtime/launch.hpp>

#include <charconv>

namespace ox {

namespace {

template <class T>
std::optional<T> parseNumber(std::string_view s) {
    T v{};
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc() || p != s.data() + s.size()) return std::nullopt;
    return v;
}

std::optional<QualityLevel> parseQuality(std::string_view s) {
    std::string cap(s);
    if (!cap.empty()) {
        for (auto& c : cap) c = char(std::tolower((unsigned char)c));
        cap[0] = char(std::toupper((unsigned char)cap[0]));
    }
    auto l = scalability::levelFromName(cap);
    if (l && *l != QualityLevel::Custom) return l;
    return std::nullopt;
}

} // namespace

std::string launchUsage() {
    return "usage: OxwaldPlayer [--project <dir|file.oxproj>] [--pak <file>] [--patch-pak <file>]... [--scene <uri>] [--headless] [--server]\n"
           "                    [--frames N] [--width W] [--height H] [--fullscreen|--borderless|--windowed]\n"
           "                    [--monitor I] [--quality low|medium|high|ultra] [--cvar name=value]...\n"
           "                    [--fps N] [--fixed-rate HZ] [--single-thread] [--user-dir DIR] [-- game args...]\n";
}

Result<LaunchOptions> parseLaunchOptions(std::span<const std::string> args) {
    LaunchOptions o;
    for (usize i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto value = [&]() -> Result<std::string> {
            if (i + 1 >= args.size()) return makeError("{} needs a value", a);
            return args[++i];
        };
        auto intValue = [&]() -> Result<i64> {
            auto v = value();
            if (!v) return v.error();
            auto n = parseNumber<i64>(*v);
            if (!n || *n < 0) return makeError("{}: '{}' is not a non-negative integer", a, *v);
            return *n;
        };
        auto floatValue = [&]() -> Result<f64> {
            auto v = value();
            if (!v) return v.error();
            auto n = parseNumber<f64>(*v);
            if (!n || *n <= 0.0) return makeError("{}: '{}' is not a positive number", a, *v);
            return *n;
        };
#define OX_TRY(expr, target)                                                                                           \
    do {                                                                                                               \
        auto r_ = (expr);                                                                                              \
        if (!r_) return r_.error();                                                                                    \
        target = *r_;                                                                                                  \
    } while (0)
        if (a == "--") {
            o.gameArgs.assign(args.begin() + i + 1, args.end());
            break;
        }
        if (a == "--project") {
            OX_TRY(value(), o.project);
        } else if (a == "--pak") {
            OX_TRY(value(), o.pak);
        } else if (a == "--patch-pak") {
            std::string patch;
            OX_TRY(value(), patch);
            o.patchPaks.emplace_back(patch);
        } else if (a == "--scene") {
            OX_TRY(value(), o.scene);
        } else if (a == "--headless") {
            o.headless = true;
        } else if (a == "--server") {
            o.server = true;
            o.headless = true;
        } else if (a == "--frames") {
            i64 n = 0;
            OX_TRY(intValue(), n);
            o.frames = u64(n);
        } else if (a == "--width" || a == "--height") {
            i64 n = 0;
            OX_TRY(intValue(), n);
            (a == "--width" ? o.width : o.height) = i32(n);
        } else if (a == "--fullscreen") {
            o.windowMode = WindowMode::Fullscreen;
        } else if (a == "--borderless") {
            o.windowMode = WindowMode::Borderless;
        } else if (a == "--windowed") {
            o.windowMode = WindowMode::Windowed;
        } else if (a == "--monitor") {
            i64 n = 0;
            OX_TRY(intValue(), n);
            o.monitor = i32(n);
        } else if (a == "--quality") {
            std::string q;
            OX_TRY(value(), q);
            o.quality = parseQuality(q);
            if (!o.quality) return makeError("--quality: unknown level '{}' (low, medium, high, ultra)", q);
        } else if (a == "--cvar") {
            std::string c;
            OX_TRY(value(), c);
            if (c.find('=') == std::string::npos) return makeError("--cvar expects name=value, got '{}'", c);
            o.cvars.push_back(std::move(c));
        } else if (a == "--fps") {
            f64 f = 0;
            OX_TRY(floatValue(), f);
            o.fps = f;
        } else if (a == "--fixed-rate") {
            f64 f = 0;
            OX_TRY(floatValue(), f);
            o.fixedRate = f;
        } else if (a == "--single-thread") {
            o.singleThread = true;
        } else if (a == "--user-dir") {
            OX_TRY(value(), o.userDir);
        } else if (a == "--help" || a == "-h") {
            o.help = true;
        } else if (!a.starts_with("-") && o.project.empty()) {
            o.project = a; // positional project path
        } else {
            return makeError("unknown option '{}'", a);
        }
#undef OX_TRY
    }
    return o;
}

Result<LaunchOptions> parseLaunchOptions(int argc, const char* const* argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    return parseLaunchOptions(args);
}

EngineConfig LaunchOptions::toEngineConfig(std::string appName) const {
    EngineConfig c;
    c.appName = std::move(appName);
    c.projectPath = project;
    c.pakPath = pak;
    c.patchPaks = patchPaks;
    c.startupScene = scene;
    c.headless = headless;
    c.dedicatedServer = server;
    c.threadedRendering = !singleThread;
    c.quality = quality;
    c.cvars = cvars;
    if (width) c.cvars.push_back("r.ResolutionX=" + std::to_string(*width));
    if (height) c.cvars.push_back("r.ResolutionY=" + std::to_string(*height));
    if (windowMode) c.cvars.push_back("r.WindowMode=" + std::to_string(int(*windowMode)));
    if (monitor) c.cvars.push_back("r.Monitor=" + std::to_string(*monitor));
    if (fps) c.targetFps = *fps;
    c.fixedRate = fixedRate;
    c.userDir = userDir;
    c.gameArgs = gameArgs;
    return c;
}

} // namespace ox
