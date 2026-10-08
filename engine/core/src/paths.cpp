#include <oxwald/core/paths.hpp>

#include <oxwald/core/log.hpp>

#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <pwd.h>
#include <unistd.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

#ifndef OX_ENGINE_SOURCE_DIR
#define OX_ENGINE_SOURCE_DIR ""
#endif

namespace ox::paths {

namespace {

// Returned as a path: on Windows the value is UTF-16 and may not be representable in the ANSI code page.
std::filesystem::path envVar(const char* name) {
#if defined(_WIN32)
    wchar_t buffer[32768];
    std::wstring wname(name, name + std::char_traits<char>::length(name));
    const DWORD n = GetEnvironmentVariableW(wname.c_str(), buffer, static_cast<DWORD>(std::size(buffer)));
    if (n == 0 || n >= std::size(buffer)) {
        return {};
    }
    return std::filesystem::path(std::wstring(buffer, n));
#else
    const char* v = std::getenv(name);
    return v ? std::filesystem::path(v) : std::filesystem::path();
#endif
}

#if !defined(_WIN32)
std::filesystem::path homeDir() {
    std::filesystem::path home = envVar("HOME");
    if (!home.empty()) {
        return home;
    }
    if (const passwd* pw = getpwuid(getuid()); pw && pw->pw_dir) {
        return pw->pw_dir;
    }
    return {};
}
#endif

} // namespace

std::filesystem::path engineSourceDir() {
    if (std::filesystem::path env = envVar("OXWALD_SOURCE_DIR"); !env.empty()) {
        return env.lexically_normal();
    }
    return std::filesystem::path(OX_ENGINE_SOURCE_DIR).lexically_normal();
}

std::filesystem::path executablePath() {
#if defined(_WIN32)
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n == 0) {
            return {};
        }
        if (n < buffer.size()) {
            return std::filesystem::path(std::wstring(buffer.data(), n));
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size + 1, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return {};
    }
    std::error_code ec;
    std::filesystem::path p(buffer.data());
    std::filesystem::path canonical = std::filesystem::weakly_canonical(p, ec);
    return ec ? p : canonical;
#else
    std::error_code ec;
    std::filesystem::path p = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path{} : p;
#endif
}

std::filesystem::path executableDir() { return executablePath().parent_path(); }

std::filesystem::path userDataDir(std::string_view appName) {
    const std::filesystem::path app{std::string(appName)};
#if defined(_WIN32)
    std::filesystem::path base = envVar("APPDATA");
    if (base.empty()) {
        base = envVar("USERPROFILE");
    }
    return base / app;
#elif defined(__APPLE__)
    return homeDir() / "Library" / "Application Support" / app;
#else
    if (std::filesystem::path xdg = envVar("XDG_DATA_HOME"); !xdg.empty()) {
        return xdg / app;
    }
    return homeDir() / ".local" / "share" / app;
#endif
}

std::filesystem::path tempDir() {
    std::error_code ec;
    std::filesystem::path p = std::filesystem::temp_directory_path(ec);
    if (ec) {
        OX_LOG_WARN("paths", "temp_directory_path failed: {}", ec.message());
#if defined(_WIN32)
        return std::filesystem::path("C:\\Temp");
#else
        return std::filesystem::path("/tmp");
#endif
    }
    return p;
}

} // namespace ox::paths
