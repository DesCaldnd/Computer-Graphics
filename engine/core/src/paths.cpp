#include <oxwald/core/paths.hpp>

#include <oxwald/core/log.hpp>

#include <cstdlib>
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

std::string envVar(const char* name) {
#if defined(_WIN32)
    wchar_t buffer[32768];
    std::wstring wname(name, name + std::char_traits<char>::length(name));
    const DWORD n = GetEnvironmentVariableW(wname.c_str(), buffer, static_cast<DWORD>(std::size(buffer)));
    if (n == 0 || n >= std::size(buffer)) {
        return {};
    }
    return std::filesystem::path(std::wstring(buffer, n)).string();
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

#if !defined(_WIN32)
std::filesystem::path homeDir() {
    std::string home = envVar("HOME");
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
    if (std::string env = envVar("OXWALD_SOURCE_DIR"); !env.empty()) {
        return std::filesystem::path(env).lexically_normal();
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
    std::string base = envVar("APPDATA");
    if (base.empty()) {
        base = envVar("USERPROFILE");
    }
    return std::filesystem::path(base) / app;
#elif defined(__APPLE__)
    return homeDir() / "Library" / "Application Support" / app;
#else
    if (std::string xdg = envVar("XDG_DATA_HOME"); !xdg.empty()) {
        return std::filesystem::path(xdg) / app;
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
