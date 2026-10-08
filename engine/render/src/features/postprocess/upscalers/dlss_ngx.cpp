// NVIDIA DLSS through the NGX Vulkan API. Compiled into ox_render only with the NGX runtime (Windows/Linux,
// OX_ENABLE_DLSS); elsewhere the ox_dlss_syntax_check object library compiles it against the SDK headers so this file
// stays buildable (it is never linked there).
//
// Flow: probe() → (device created with the NGX extensions?) NVSDK_NGX_VULKAN_Init_with_ProjectID +
// GetCapabilityParameters + SuperSampling.Available; optimalSettings() → NGX_DLSS_GET_OPTIMAL_SETTINGS; evaluate() →
// (re)create the per-view feature with NGX_VULKAN_CREATE_DLSS_EXT1 (MVLowRes | IsHDR | DepthInverted
// [| AutoExposure]) and NGX_VULKAN_EVALUATE_DLSS_EXT; release() → NVSDK_NGX_VULKAN_ReleaseFeature; shutdown() →
// DestroyParameters / Shutdown1, from a rhi::Device shutdown callback: NGX keeps Vulkan objects on the device for as
// long as it is initialised, whoever probed it (a Renderer or just upscalerAvailability()).
#include "dlss_backend.hpp"

#include <oxwald/core/log.hpp>
#include <oxwald/core/paths.hpp>
#include <oxwald/rhi/command_list.hpp>
#include <oxwald/rhi/device.hpp>
#include <oxwald/rhi/vulkan.hpp>

// The NGX headers rely on the Vulkan types volk (VK_NO_PROTOTYPES) has already declared.
#if defined(_MSC_VER)
#pragma warning(disable : 4389) // NVSDK_NGX_FAILED/SUCCEED compare an unsigned mask with an enumerator
#endif
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_vk.h>

#include <atomic>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>
#include <string_view>

namespace ox::render::dlss {

namespace {

// NGX project id for engines without an NVIDIA application id (NVSDK_NGX_ENGINE_TYPE_CUSTOM).
constexpr const char* kProjectId = "6f1e3b8a-5c2d-4f7e-9a10-0b5e7d0a4c21";
constexpr const char* kEngineVersion = "0.1";

struct NgxState {
    std::mutex mutex;
    VkDevice device = VK_NULL_HANDLE;
    bool probed = false;
    bool initialized = false;
    Status status;
    NVSDK_NGX_Parameter* params = nullptr;
    std::wstring dataPath;
    std::wstring featurePath; // executable directory
    std::wstring cwdPath;
};

NgxState& state() {
    static NgxState s;
    return s;
}
std::atomic<bool> g_unavailable{false};

std::string resultText(NVSDK_NGX_Result r) {
    const wchar_t* w = GetNGXResultAsString(r);
    std::string s;
    if (w) {
        for (; *w; ++w) s += *w < 128 ? char(*w) : '?';
    }
    return std::format("{} (0x{:08x})", s.empty() ? "NGX error" : s, u32(r));
}

NVSDK_NGX_PerfQuality_Value perfQuality(UpscalerQuality q) {
    switch (q) {
    case UpscalerQuality::UltraPerformance: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    case UpscalerQuality::Performance: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    case UpscalerQuality::Balanced: return NVSDK_NGX_PerfQuality_Value_Balanced;
    case UpscalerQuality::Quality: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    case UpscalerQuality::Native: return NVSDK_NGX_PerfQuality_Value_DLAA;
    }
    return NVSDK_NGX_PerfQuality_Value_MaxQuality;
}

NVSDK_NGX_Resource_VK imageResource(rhi::Device& device, rhi::TextureHandle t, bool readWrite) {
    const rhi::TextureDesc& d = device.desc(t);
    VkImageSubresourceRange range{};
    const bool depth = d.format == VK_FORMAT_D32_SFLOAT || d.format == VK_FORMAT_D24_UNORM_S8_UINT ||
                       d.format == VK_FORMAT_D32_SFLOAT_S8_UINT || d.format == VK_FORMAT_D16_UNORM;
    range.aspectMask = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;
    return NVSDK_NGX_Create_ImageView_Resource_VK(device.view(t), device.vkImage(t), range, d.format, d.width,
                                                  d.height, readWrite);
}

void releaseLocked(ViewFeature& f) {
    if (f.handle) NVSDK_NGX_VULKAN_ReleaseFeature(static_cast<NVSDK_NGX_Handle*>(f.handle));
    f = {};
}

} // namespace

bool compiled() { return true; }
std::string buildUnavailableReason() { return {}; }
bool knownUnavailable() { return g_unavailable.load(); }

void requiredExtensions(std::vector<std::string>& instanceExtensions, std::vector<std::string>& deviceExtensions) {
    unsigned int instCount = 0, devCount = 0;
    const char** inst = nullptr;
    const char** dev = nullptr;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_RequiredExtensions(&instCount, &inst, &devCount, &dev))) return;
    for (unsigned int i = 0; i < instCount; ++i) instanceExtensions.emplace_back(inst[i]);
    for (unsigned int i = 0; i < devCount; ++i) {
        // rhi enables the core 1.2 bufferDeviceAddress feature, which must not be combined with the EXT extension
        // (VUID-VkDeviceCreateInfo-pNext-04748).
        if (std::string_view(dev[i]) == "VK_EXT_buffer_device_address") continue;
        deviceExtensions.emplace_back(dev[i]);
    }
}

namespace {

// NGX creates its pipelines with whatever NVSDK_NGX_VULKAN_RequiredExtensions lists (push descriptors, NVX binary
// import, ...) without checking the device: calling it on a device that lacks them only produces validation errors
// and NVSDK_NGX_Result_FAIL_PlatformError, so the check is done here.
std::string missingExtensions(const rhi::Device& device) {
    std::vector<std::string> inst, dev;
    requiredExtensions(inst, dev);
    std::string missing;
    auto add = [&](const std::string& e) { missing += (missing.empty() ? "" : ", ") + e; };
    for (const std::string& e : inst) {
        if (!device.hasInstanceExtension(e)) add(e);
    }
    for (const std::string& e : dev) {
        if (!device.hasDeviceExtension(e)) add(e);
    }
    return missing;
}

// Releases everything NGX holds on the device (state().mutex held).
void shutdownLocked(NgxState& s, VkDevice device) {
    if (s.params) NVSDK_NGX_VULKAN_DestroyParameters(s.params);
    s.params = nullptr;
    if (s.initialized) NVSDK_NGX_VULKAN_Shutdown1(device);
    s.initialized = false;
}

Status probeLocked(NgxState& s, rhi::Device& device) {
    Status st;
    if (device.caps().vendor != rhi::GpuVendor::Nvidia) {
        st.reason = std::format("NVIDIA DLSS requires an NVIDIA RTX GPU (this GPU: {})", device.caps().gpuName);
        return st;
    }
    if (const std::string missing = missingExtensions(device); !missing.empty()) {
        st.reason = "the Vulkan device was created without the extensions NVIDIA NGX needs (" + missing +
                    "): call render::appendUpscalerVulkanExtensions(DeviceDesc&) before rhi::Device::create";
        return st;
    }
    std::error_code ec;
    const std::filesystem::path data = std::filesystem::temp_directory_path(ec) / "oxwald_ngx";
    std::filesystem::create_directories(data, ec);
    s.dataPath = data.wstring();
    // The DLSS feature library (nvngx_dlss.dll / libnvidia-ngx-dlss.so.*) is deployed next to the executable by
    // ox_deploy_vulkan_runtime; the working directory is only a fallback (tools started from elsewhere).
    s.featurePath = paths::executableDir().wstring();
    s.cwdPath = std::filesystem::current_path(ec).wstring();
    const wchar_t* searchPaths[] = {s.featurePath.c_str(), s.cwdPath.c_str()};
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = searchPaths;
    info.PathListInfo.Length = s.featurePath.empty() ? 0 : 1;
    if (!s.cwdPath.empty() && s.cwdPath != s.featurePath) {
        if (info.PathListInfo.Length == 0) searchPaths[0] = s.cwdPath.c_str();
        ++info.PathListInfo.Length;
    }
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init_with_ProjectID(
        kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion, s.dataPath.c_str(), device.vkInstance(),
        device.vkPhysicalDevice(), device.vkDevice(), vkGetInstanceProcAddr, vkGetDeviceProcAddr, &info);
    if (NVSDK_NGX_FAILED(r)) {
        st.reason = "NGX initialisation failed: " + resultText(r);
        return st;
    }
    s.initialized = true;
    r = NVSDK_NGX_VULKAN_GetCapabilityParameters(&s.params);
    if (NVSDK_NGX_FAILED(r) || !s.params) {
        st.reason = "NGX capability parameters unavailable: " + resultText(r);
        return st;
    }
    int needsDriver = 0;
    unsigned int minMajor = 0, minMinor = 0;
    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetI(s.params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver,
                                                    &needsDriver)) &&
        needsDriver) {
        NVSDK_NGX_Parameter_GetUI(s.params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &minMajor);
        NVSDK_NGX_Parameter_GetUI(s.params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minMinor);
        st.reason = std::format("NVIDIA DLSS needs a newer driver (at least {}.{})", minMajor, minMinor);
        return st;
    }
    int available = 0;
    r = NVSDK_NGX_Parameter_GetI(s.params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    if (NVSDK_NGX_FAILED(r) || !available) {
        int initResult = 0;
        NVSDK_NGX_Parameter_GetI(s.params, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &initResult);
        st.reason = "NVIDIA DLSS is not supported on this GPU/driver: " +
                    resultText(NVSDK_NGX_Result(initResult ? initResult : int(r)));
        return st;
    }
    st.available = true;
    return st;
}

} // namespace

Status probe(rhi::Device& device) {
    NgxState& s = state();
    std::lock_guard lock(s.mutex);
    if (s.probed && s.device == device.vkDevice()) return s.status;
    if (s.probed) {
        // volk allows one rhi::Device at a time and its shutdown callback forgets it: not reachable in practice.
        return {false, "NVIDIA NGX is bound to another Vulkan device"};
    }
    s.probed = true;
    s.device = device.vkDevice();
    s.status = probeLocked(s, device);
    g_unavailable = !s.status.available;
    if (s.status.available) OX_LOG_INFO("render", "NVIDIA DLSS available (NGX initialised)");
    else shutdownLocked(s, s.device); // NGX came up but DLSS cannot run: keep nothing on the device
    // The verdict (and NGX itself) lives exactly as long as the device.
    device.addShutdownCallback([&device] { shutdown(device); });
    return s.status;
}

OptimalSettings optimalSettings(rhi::Device& device, u32 outputWidth, u32 outputHeight, UpscalerQuality quality) {
    OptimalSettings o;
    if (!probe(device).available) return o;
    NgxState& s = state();
    std::lock_guard lock(s.mutex);
    unsigned int w = 0, h = 0, maxW = 0, maxH = 0, minW = 0, minH = 0;
    float sharpness = 0.0f;
    const NVSDK_NGX_Result r = NGX_DLSS_GET_OPTIMAL_SETTINGS(s.params, outputWidth, outputHeight, perfQuality(quality),
                                                             &w, &h, &maxW, &maxH, &minW, &minH, &sharpness);
    if (NVSDK_NGX_FAILED(r) || w == 0 || h == 0) return o;
    o.valid = true;
    o.renderWidth = w;
    o.renderHeight = h;
    o.sharpness = sharpness;
    return o;
}

bool evaluate(rhi::Device& device, rhi::CommandList& cmd, ViewFeature& f, const EvaluateParams& p) {
    if (!probe(device).available) return false;
    NgxState& s = state();
    std::lock_guard lock(s.mutex);
    int flags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                NVSDK_NGX_DLSS_Feature_Flags_DepthInverted; // reversed-Z
    if (!p.exposure) flags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    const bool recreate = !f.handle || f.renderWidth != p.renderWidth || f.renderHeight != p.renderHeight ||
                          f.outputWidth != p.outputWidth || f.outputHeight != p.outputHeight ||
                          f.quality != i32(p.quality) || f.flags != flags;
    if (recreate) {
        if (f.handle) {
            device.waitIdle(); // the previous feature may still be referenced by frames in flight
            releaseLocked(f);
        }
        NVSDK_NGX_DLSS_Create_Params cp{};
        cp.Feature.InWidth = p.renderWidth;
        cp.Feature.InHeight = p.renderHeight;
        cp.Feature.InTargetWidth = p.outputWidth;
        cp.Feature.InTargetHeight = p.outputHeight;
        cp.Feature.InPerfQualityValue = perfQuality(p.quality);
        cp.InFeatureCreateFlags = flags;
        NVSDK_NGX_Handle* handle = nullptr;
        const NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSS_EXT1(device.vkDevice(), cmd.vk(), 1, 1, &handle, s.params, &cp);
        if (NVSDK_NGX_FAILED(r) || !handle) {
            OX_LOG_ERROR("render", "DLSS feature creation failed: {}", resultText(r));
            return false;
        }
        f.handle = handle;
        f.renderWidth = p.renderWidth;
        f.renderHeight = p.renderHeight;
        f.outputWidth = p.outputWidth;
        f.outputHeight = p.outputHeight;
        f.quality = i32(p.quality);
        f.flags = flags;
    }
    NVSDK_NGX_Resource_VK color = imageResource(device, p.color, false);
    NVSDK_NGX_Resource_VK depth = imageResource(device, p.depth, false);
    NVSDK_NGX_Resource_VK motion = imageResource(device, p.motion, false);
    NVSDK_NGX_Resource_VK output = imageResource(device, p.output, true);
    NVSDK_NGX_Resource_VK exposure{};
    if (p.exposure) exposure = imageResource(device, p.exposure, false);

    NVSDK_NGX_VK_DLSS_Eval_Params e{};
    e.Feature.pInColor = &color;
    e.Feature.pInOutput = &output;
    e.Feature.InSharpness = p.sharpness; // ignored by current DLSS models (NGX sharpening is deprecated)
    e.pInDepth = &depth;
    e.pInMotionVectors = &motion;
    e.pInExposureTexture = p.exposure ? &exposure : nullptr;
    // Conventions verified on an RTX 3080 (NGX 310.9.1), see PostProcessTest.DlssStaticSceneConvergesAndStaysStable /
    // DlssFollowsCameraMotion: flipping the sign of either jitter or either motion vector axis loses 2.5–15 dB.
    // Jitter: the offset in render pixels (x right, y down) by which the jittered projection moves the image, i.e.
    // RenderView::jitterPixels() as is (proj = translate(jitter * 2 / renderSize) * unjittered, NDC y down).
    e.InJitterOffsetX = p.jitterPixels.x;
    e.InJitterOffsetY = p.jitterPixels.y;
    e.InRenderSubrectDimensions = {p.renderWidth, p.renderHeight};
    e.InReset = p.reset ? 1 : 0;
    // Velocity is uvCurrent - uvPrevious (unjittered, uv y down, render resolution: MVLowRes without MVJittered);
    // NGX wants the offset in render pixels from the current to the previous position.
    e.InMVScaleX = -f32(p.renderWidth);
    e.InMVScaleY = -f32(p.renderHeight);
    // Colour is radiance × preExposure and the texture holds the absolute exposure: NGX evaluates
    // colour / InPreExposure × exposure. Only presets J/K (Quality, Balanced, DLAA by default) read them; L/M
    // (UltraPerformance, Performance) always use NGX's auto exposure.
    e.InPreExposure = p.preExposure;
    e.InExposureScale = 1.0f;
    e.InFrameTimeDeltaInMsec = p.frameTimeMs;
    const NVSDK_NGX_Result r =
        NGX_VULKAN_EVALUATE_DLSS_EXT(cmd.vk(), static_cast<NVSDK_NGX_Handle*>(f.handle), s.params, &e);
    if (NVSDK_NGX_FAILED(r)) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) OX_LOG_ERROR("render", "DLSS evaluation failed: {}", resultText(r));
        return false;
    }
    return true;
}

void release(ViewFeature& f) {
    std::lock_guard lock(state().mutex);
    releaseLocked(f);
}

void shutdown(rhi::Device& device) {
    NgxState& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.probed || s.device != device.vkDevice()) return;
    shutdownLocked(s, s.device);
    s.probed = false;
    s.device = VK_NULL_HANDLE;
    s.status = {};
    g_unavailable = false; // the next device is probed afresh
}

} // namespace ox::render::dlss
