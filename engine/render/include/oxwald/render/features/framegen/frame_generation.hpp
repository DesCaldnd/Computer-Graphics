#pragma once

// Frame generation: AMD FidelityFX FSR 3.1 frame interpolation (ffx-api, prebuilt amd_fidelityfx_vk.dll of
// FidelityFX SDK v1.1.4 — the last release with a Vulkan backend; vcpkg overlay port amd-fidelityfx-vk).
//
// For every rendered frame the FSR 3 swapchain proxy presents one interpolated frame first (between the previous
// and the current rendered frame) and then the rendered one, paced by its own threads. The engine:
//   - creates the device with three extra queues (rhi::DeviceDesc::frameGenerationQueues) and the KHR extensions
//     the FidelityFX Vulkan backend loads functions from (appendVulkanRequirements);
//   - routes rhi::Swapchain through the proxy (rhi::ISwapchainProxy) while r.FrameGeneration is on;
//   - records the "prepare" dispatch (depth + motion vectors at render resolution, camera) and copies the HUD-less
//     image (SceneColorLDR before editor overlays and UI) at the Overlay injection point of the main view; the UI
//     is detected from the HUD-less difference and kept sharp;
//   - configures the proxy every frame (frame id, HUD-less resource, on/off).
// Works with every upscaler (Off/FSR1/TAAU/DLSS: inputs are the render-resolution depth and velocity) and any vendor.
// The runtime player only: the editor viewport never presents through the proxy. NVIDIA Reflex is not used while
// the proxy owns the swapchain (the engine latency limiter is). See docs/dev/modules/render_postprocess.md.

#include <oxwald/core/types.hpp>

#include <memory>
#include <string>
#include <vector>


namespace ox::rhi {
class Device;
class Swapchain;
struct DeviceDesc;
} // namespace ox::rhi

namespace ox::render {

class Renderer;

namespace framegen {

// r.FrameGeneration (Persist): 0 Off, 1 FSR3.
[[nodiscard]] i32 requestedMode();
void registerCVars();

// Windows with the FidelityFX runtime next to the executable: requests the queues and extensions the proxy needs.
// No-op elsewhere. Call before rhi::Device::create (windowed devices only).
void appendVulkanRequirements(rhi::DeviceDesc& desc);

struct Availability {
    bool available = false;
    std::string reason; // empty when available
};
[[nodiscard]] Availability availability(const rhi::Device& device);

struct Stats {
    bool active = false;        // the proxy owns the swapchain and interpolation is on
    u64 renderedFrames = 0;     // frames rendered and presented while active
    u64 presentedFrames = 0;    // frames the proxy presented over the same period (rendered + generated)
    // Frames the proxy composed for its own presents, by kind (present thread). The proxy's pacer drops a whole
    // scheduled pair (generated + rendered) when its present thread falls behind (FidelityFX SDK 1.1.4,
    // composeAndPresent_presenterThread), so presentedFrames / renderedFrames dips below 2 on a loaded machine while
    // generatedPresented / realPresented stays 1.
    u64 generatedPresented = 0;
    u64 realPresented = 0;
    // Our pacing guard (Fsr3FrameGenerator::present): frame N is handed to the proxy only after its present thread
    // composed frame N - 1 (FidelityFX 1.1.4 deadlocks after dropping a scheduled pair). Average wait (ms) and the
    // number of 250 ms timeouts (the present thread composed nothing: minimised window, no image acquired).
    f64 pacingWaitMs = 0.0;
    u64 pacingTimeouts = 0;
    f64 lastConfigureMs = 0.0;  // CPU cost of the per-frame configure call
    // Average time a rendered frame is held by the proxy: from the engine's present call to the proxy composing it for
    // its own present (after the generated frame before it). The latency frame generation adds (CPU timestamps; the
    // proxy's pacing wait after composition is not included).
    f64 addedLatencyMs = 0.0;
    std::string status;         // human readable state / last error
};

class FrameGenerator {
public:
    // Null with a reason when unavailable (no DLL, no queues, headless).
    static std::unique_ptr<FrameGenerator> create(rhi::Device& device, Renderer& renderer, u32 mainView,
                                                  std::string* reason = nullptr);
    virtual ~FrameGenerator() = default;

    // Render thread, before Swapchain::acquire: enables / disables the proxy (applied by that acquire).
    virtual void beginFrame(rhi::Swapchain& swapchain, bool enable) = 0;
    // Render thread, after a successful acquire and before Renderer::renderView: configures this frame (frame id,
    // HUD-less image) and arms the prepare pass of the main view. frameTimeMs: time since the previous frame.
    virtual void prepareFrame(rhi::Swapchain& swapchain, f32 frameTimeMs) = 0;
    // Render thread, after Swapchain::present.
    virtual void endFrame(rhi::Swapchain& swapchain) = 0;
    // Render thread: true while frames are presented through the proxy.
    [[nodiscard]] virtual bool proxyActive() const = 0;
    [[nodiscard]] virtual Stats stats() const = 0;
    // Before the swapchain is destroyed: leaves the proxy (destroying its contexts).
    virtual void release(rhi::Swapchain& swapchain) = 0;

    // Tests / tools: copies the next *generated* frame the proxy presents (back buffer bytes, tightly packed, in the
    // swapchain format) into host memory. Render thread; poll capturedFrame() after a few presents.
    struct CapturedFrame {
        bool done = false;
        u64 frameId = 0; // generated between rendered frames frameId - 1 and frameId (ids count from 1)
        u32 width = 0, height = 0;
        std::vector<u8> pixels;
    };
    virtual void captureNextGeneratedFrame() = 0;
    [[nodiscard]] virtual CapturedFrame capturedFrame() = 0;
    // Frame id of the frame prepared by the last prepareFrame() (1, 2, ...).
    [[nodiscard]] virtual u64 currentFrameId() const = 0;
};

} // namespace framegen
} // namespace ox::render
