#pragma once

#include <oxwald/core/result.hpp>
#include <oxwald/core/types.hpp>

#include <glm/vec2.hpp>

#include <array>
#include <atomic>
#include <filesystem>
#include <functional>
#include <string_view>

namespace ox {
class Services;
class World;
namespace rhi {
class ISurfaceProvider; // rhi/swapchain.hpp; runtime only passes the pointer through
}

// Where the renderer presents. All members may be null/zero: a headless renderer (tests, dedicated servers,
// offline rendering) gets an empty surface.
struct RenderSurface {
    rhi::ISurfaceProvider* provider = nullptr; // Vulkan surface factory (GlfwSurfaceProvider, Qt viewport, ...)
    void* nativeWindow = nullptr;              // GLFWwindow* / QWindow* — informational
    glm::uvec2 framebufferSize{0, 0};          // pixels
    f32 dpiScale = 1.0f;

    [[nodiscard]] bool headless() const { return provider == nullptr; }
};

// Number of renderer-owned snapshot slots. The game thread extracts frame N+1 into one slot while the render
// thread draws frame N from the other.
inline constexpr u32 kRenderSnapshotSlots = 2;

// Per-frame data handed to extract() and (copied) to the matching render().
struct FrameContext {
    u64 frameIndex = 0;     // monotonically increasing, starts at 0
    u32 slot = 0;           // snapshot slot (frameIndex % kRenderSnapshotSlots)
    f64 time = 0.0;         // scaled game time in seconds
    f64 realTime = 0.0;     // unscaled wall time since engine start
    f32 dt = 0.0f;          // scaled frame delta
    f32 realDt = 0.0f;      // unscaled frame delta
    f32 alpha = 0.0f;       // fixed-step interpolation factor [0,1): lerp(previous, current, alpha)
    f32 timeScale = 1.0f;
    bool paused = false;
    bool editMode = false;  // editor edit mode (no simulation)
    bool loading = false;   // a level is being loaded (draw a loading screen)
    glm::uvec2 viewportSize{0, 0};
};

// Low latency: NVIDIA Reflex (VK_NV_low_latency2) or the engine's fallback limiter (r.Reflex, r.Reflex.Fallback;
// docs/dev/modules/runtime.md "Latency"). Game thread markers; the renderer adds render submit / present markers.
enum class LatencyMarker : u8 { SimulationStart, SimulationEnd, InputSample };

struct LatencyStats {
    i32 mode = 0;                 // r.Reflex as applied: 0 Off, 1 On, 2 On + Boost
    bool reflexSupported = false; // VK_NV_low_latency2 on this swapchain
    // The device has VK_NV_low_latency2 but the FSR 3 frame generation proxy owns the swapchain (Reflex cannot drive
    // the proxy's swapchain; r.Reflex then uses the engine limiter, see docs/dev/modules/runtime.md "Latency").
    bool reflexBehindFrameGeneration = false;
    bool reflexActive = false;    // driver latency sleep in use this frame
    bool limiterActive = false;   // engine fallback limiter in use this frame
    f64 sleepMs = 0.0;            // last latency sleep (driver or limiter), game thread
    // Render-side queueing of the last frame: swapchain acquire + GPU queueing behind the previous frame (previous
    // frame done - this frame submitted, measured by the renderer on the CPU; negative when the GPU idled). Input of
    // the fallback limiter (with EngineStats::waitForRenderMs and snapshotQueuedMs).
    f64 renderQueueWaitMs = 0.0;
    // Simulation start -> GPU finished the frame, measured by the engine (CPU timestamps: the game thread marker and a
    // thread waiting on the graphics timeline). Any vendor, with or without Reflex / frame generation; averaged.
    f64 engineLatencyMs = 0.0;
    // Averages over the driver's latest reports (vkGetLatencyTimingsNV; 0 without Reflex support):
    u32 reports = 0;
    f64 pcLatencyMs = 0.0;        // simulation start -> GPU render end
    f64 gameToRenderMs = 0.0;     // simulation start -> render submit start
    f64 renderQueueMs = 0.0;      // render submit end -> GPU render start
    f64 gpuRenderMs = 0.0;        // GPU render start -> end
};

struct RenderStats {
    u64 framesRendered = 0;
    f64 lastRenderMs = 0.0;
    f64 lastExtractMs = 0.0;
    u64 drawCalls = 0;
    // Breakdown of lastRenderMs (renderers that know it; 0 otherwise). Render thread view of one frame:
    f64 lastFrameWaitMs = 0.0; // waiting for the GPU to retire the frame slot (frames in flight full: GPU bound)
    f64 lastAcquireMs = 0.0;   // swapchain image acquire (blocks when the presentation engine holds every image)
    f64 lastPresentMs = 0.0;   // queue present (FIFO/vsync back-pressure shows up here or in acquire)
    f64 lastCpuRenderMs = 0.0; // renderer CPU work (setup + recording + submission)
    f64 lastGpuFrameMs = 0.0;  // GPU time of the last retired frame (timestamps), 0 without timestamp queries
    bool vsync = false;        // the presentation engine waits for vblank (FIFO)
    LatencyStats latency;
    struct {
        bool active = false;      // r.FrameGeneration: presenting through the FSR 3 proxy with interpolation on
        u64 renderedFrames = 0;   // since activation
        u64 presentedFrames = 0;  // rendered + generated, since activation
        f64 addedLatencyMs = 0.0; // average hold of a rendered frame in the proxy (present call -> its own present)
    } frameGeneration;

};

// Renderer contract (implemented by the render module; NullRenderer here).
//
// Threading (see RenderPipeline):
//   init/shutdown          — game (main) thread, render thread not running.
//   extract(world, ctx)    — game thread, after PostUpdate/Extract systems. Copy everything render() needs into the
//                            renderer-owned snapshot slot ctx.slot. Must not keep references into the World.
//   render(ctx)            — render thread (or the game thread in single-threaded mode). Reads only snapshot
//                            ctx.slot (+ renderer-private GPU state). Never touches the World or game services.
//   resize/settingsChanged — called on the render thread right before a render() (never concurrently with it).
// The pipeline guarantees: extract(slot s) never overlaps render(slot s); renders happen in frame order; frame
// N+1 may be extracted while frame N renders (different slots); the game thread is at most one frame ahead.
class IRenderer {
public:
    virtual ~IRenderer() = default;

    [[nodiscard]] virtual std::string_view name() const = 0;
    virtual Status init(Services& services, const RenderSurface& surface) = 0;
    virtual void shutdown() = 0;

    virtual void extract(const World& world, const FrameContext& ctx) = 0;
    virtual void render(const FrameContext& ctx) = 0;

    virtual void resize(glm::uvec2 framebufferSize) = 0;
    // Graphics cvars/scalability changed (resolution, vsync, quality, ray tracing, upscaler, ...). Rebuild what is
    // needed (render graph, swapchain) without a restart.
    virtual void settingsChanged() = 0;

    [[nodiscard]] virtual RenderStats stats() const { return {}; }

    // Low latency hooks, game thread (Engine::frame). The present id of frame N is N + 1 on both threads.
    // latencySleep: called before frame `frameIndex` samples input; the renderer may block until the driver says to
    // start (NVIDIA Reflex). Returns the ms slept, or a negative value when the renderer does not pace frames (the
    // engine then runs its fallback limiter when r.Reflex.Fallback is on).
    virtual f64 latencySleep(u64 frameIndex, i32 mode) {
        (void)frameIndex;
        (void)mode;
        return -1.0;
    }
    virtual void latencyMarker(u64 frameIndex, LatencyMarker marker) {
        (void)frameIndex;
        (void)marker;
    }
    // Blocks until the GPU finished frame `frameIndex` (already rendered by render()). False when the renderer has
    // no GPU timeline (the strict fallback limiter then only waits for the render thread).
    virtual bool waitForFrameGpu(u64 frameIndex) {
        (void)frameIndex;
        return false;
    }

    // Optional: write the next rendered frame to a PNG (any thread). `done(ok)` is called on the render thread.
    // Returns false when this renderer cannot capture (the default; the render module captures headless and windowed).
    virtual bool requestScreenshot(std::filesystem::path pngPath, std::function<void(bool ok)> done = {}) {
        (void)pngPath;
        (void)done;
        return false;
    }
};

// Does nothing but count frames; used by headless servers, tests and the player until the render module lands.
// Optionally simulates GPU time so pacing/threading can be exercised.
class NullRenderer final : public IRenderer {
public:
    explicit NullRenderer(f64 simulatedRenderMs = 0.0) : m_simulatedRenderMs(simulatedRenderMs) {}

    [[nodiscard]] std::string_view name() const override { return "Null"; }
    Status init(Services&, const RenderSurface& surface) override;
    void shutdown() override {}
    void extract(const World& world, const FrameContext& ctx) override;
    void render(const FrameContext& ctx) override;
    void resize(glm::uvec2 size) override { m_size = size; }
    void settingsChanged() override { m_settingsChanges.fetch_add(1); }
    [[nodiscard]] RenderStats stats() const override;

    [[nodiscard]] glm::uvec2 size() const { return m_size; }
    [[nodiscard]] u64 settingsChanges() const { return m_settingsChanges.load(); }

private:
    struct Snapshot {
        u64 frameIndex = 0;
        usize entityCount = 0;
    };
    f64 m_simulatedRenderMs;
    std::array<Snapshot, kRenderSnapshotSlots> m_snapshots{};
    glm::uvec2 m_size{0, 0}; // render thread
    std::atomic<u64> m_settingsChanges{0};
    std::atomic<u64> m_frames{0};
    std::atomic<f64> m_lastRenderMs{0.0};
};

} // namespace ox
