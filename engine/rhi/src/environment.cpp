#include <oxwald/core/log.hpp>
#include <oxwald/rhi/environment.hpp>

#include <cstdlib>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace ox::rhi {

namespace fs = std::filesystem;

namespace {

std::mutex g_loaderMutex;
bool g_loaderTried = false;
bool g_loaderOk = false;
std::string g_loaderPath;
void* g_loaderLib = nullptr;

bool envSet(const char* name) {
    const char* v = std::getenv(name);
    return v && *v;
}

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
#else
    ::setenv(name, value.c_str(), 0);
#endif
}

#if defined(_WIN32)
constexpr const char* kLoaderNames[] = {"vulkan-1.dll"};
#elif defined(__APPLE__)
constexpr const char* kLoaderNames[] = {"libvulkan.1.dylib", "libvulkan.dylib"};
#else
constexpr const char* kLoaderNames[] = {"libvulkan.so.1", "libvulkan.so"};
#endif

void* openLibrary(const fs::path& p) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(LoadLibraryW(p.wstring().c_str()));
#else
    return dlopen(p.string().c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void* librarySymbol(void* lib, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}

} // namespace

fs::path executableDirectory() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(std::wstring(buf, n)).parent_path();
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        std::error_code ec;
        fs::path p = fs::weakly_canonical(fs::path(buf), ec);
        return (ec ? fs::path(buf) : p).parent_path();
    }
    return fs::current_path();
#else
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::current_path() : p.parent_path();
#endif
}

void configureVulkanEnvironment() {
    const fs::path bundled = executableDirectory() / "vulkan";
    std::error_code ec;
#if defined(__APPLE__)
    if (!envSet("MVK_CONFIG_LOG_LEVEL")) {
        setEnv("MVK_CONFIG_LOG_LEVEL", "2"); // MoltenVK: warnings and errors only (info dumps the whole GPU)
    }
    if (!envSet("VK_DRIVER_FILES") && !envSet("VK_ICD_FILENAMES")) {
        fs::path icd = bundled / "MoltenVK_icd.json";
#ifdef OX_RHI_DEV_ICD_JSON
        if (!fs::exists(icd, ec)) {
            icd = OX_RHI_DEV_ICD_JSON;
        }
#endif
        if (fs::exists(icd, ec)) {
            setEnv("VK_DRIVER_FILES", icd.string());
        }
    }
#endif
    if (!envSet("VK_ADD_LAYER_PATH")) {
        fs::path layerDir = bundled;
        if (!fs::exists(layerDir / "VkLayer_khronos_validation.json", ec)) {
            layerDir.clear();
#ifdef OX_RHI_DEV_LAYER_DIR
            if (fs::exists(fs::path(OX_RHI_DEV_LAYER_DIR) / "VkLayer_khronos_validation.json", ec)) {
                layerDir = OX_RHI_DEV_LAYER_DIR;
            }
#endif
        }
        if (!layerDir.empty()) {
            setEnv("VK_ADD_LAYER_PATH", layerDir.string());
        }
    }
}

bool initializeVulkanLoader() {
    std::lock_guard lock(g_loaderMutex);
    if (g_loaderTried) {
        return g_loaderOk;
    }
    g_loaderTried = true;
    configureVulkanEnvironment();

    std::vector<fs::path> candidates;
    const fs::path bundled = executableDirectory() / "vulkan";
    for (const char* n : kLoaderNames) {
        candidates.push_back(bundled / n);
    }
#ifdef OX_RHI_DEV_LOADER_LIB
    candidates.emplace_back(OX_RHI_DEV_LOADER_LIB);
#endif
    for (const char* n : kLoaderNames) {
        candidates.emplace_back(n); // system search path
    }
    std::error_code ec;
    for (const fs::path& c : candidates) {
        if (c.has_parent_path() && !fs::exists(c, ec)) {
            continue;
        }
        void* lib = openLibrary(c);
        if (!lib) {
            continue;
        }
        auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(librarySymbol(lib, "vkGetInstanceProcAddr"));
        if (!gipa) {
            continue;
        }
        volkInitializeCustom(gipa);
        g_loaderLib = lib;
        g_loaderPath = c.string();
        g_loaderOk = true;
        OX_LOG_INFO("rhi", "Vulkan loader: {}", g_loaderPath);
        return true;
    }
    if (volkInitialize() == VK_SUCCESS) {
        g_loaderOk = true;
        g_loaderPath = "<volk default>";
        return true;
    }
    OX_LOG_ERROR("rhi", "Vulkan loader library not found");
    return false;
}

PFN_vkGetInstanceProcAddr loaderGetInstanceProcAddr() {
    return initializeVulkanLoader() ? vkGetInstanceProcAddr : nullptr;
}

std::string loaderLibraryPath() {
    std::lock_guard lock(g_loaderMutex);
    return g_loaderPath;
}

} // namespace ox::rhi
