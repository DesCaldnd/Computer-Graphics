#pragma once

#include <oxwald/core/result.hpp>
#include <oxwald/core/types.hpp>

#include <glm/vec2.hpp>

#include <array>
#include <atomic>
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

struct RenderStats {
    u64 framesRendered = 0;
    f64 lastRenderMs = 0.0;
    f64 lastExtractMs = 0.0;
    u64 drawCalls = 0;
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
