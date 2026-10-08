#pragma once

#include <oxwald/runtime/renderer.hpp>

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>

namespace ox {

// Game thread → render thread handoff with kRenderSnapshotSlots (2) renderer-owned snapshot slots.
//
//   game thread : | sim N | extract N (slot 0) | sim N+1 | extract N+1 (slot 1) | sim N+2 | wait slot 0 | extract N+2
//   render thread:                              | render N (slot 0)                          | render N+1 (slot 1) ...
//
// Slot states move Free → Extracting (game) → Ready → Rendering (render) → Free under one mutex, so every write the
// renderer makes during extract() happens-before the render() that reads it (and vice versa). Single-threaded
// mode calls extract()+render() inline on the game thread (editor/debugging).
class RenderPipeline {
public:
    enum class Mode { SingleThreaded, Threaded };

    struct Stats {
        u64 framesSubmitted = 0;
        u64 framesRendered = 0;
        f64 lastExtractMs = 0.0;
        f64 lastRenderMs = 0.0;   // render thread time of the last rendered frame
        f64 lastWaitMs = 0.0;     // game thread time spent waiting for a free slot (render-bound indicator)
    };

    RenderPipeline() = default;
    ~RenderPipeline();
    RenderPipeline(const RenderPipeline&) = delete;
    RenderPipeline& operator=(const RenderPipeline&) = delete;

    // Renderer must be initialised and outlive stop().
    void start(IRenderer& renderer, Mode mode);
    // Renders everything already submitted, then joins the render thread.
    void stop();
    [[nodiscard]] bool running() const { return m_renderer != nullptr; }
    [[nodiscard]] Mode mode() const { return m_mode; }

    // Game thread. Fills ctx.slot, waits for that slot to be free, extracts and queues the frame.
    void submit(const World& world, FrameContext ctx);
    // Game thread. Blocks until every submitted frame has been rendered.
    void flush();

    // Any thread; applied on the render thread before the next render().
    void requestResize(glm::uvec2 size);
    void requestSettingsChanged();

    [[nodiscard]] Stats stats() const;

private:
    enum class SlotState : u8 { Free, Extracting, Ready, Rendering };
    struct Queued {
        FrameContext ctx;
    };

    void renderLoop();
    void applyPendingCommands(); // render side

    IRenderer* m_renderer = nullptr;
    Mode m_mode = Mode::Threaded;
    std::thread m_thread;

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::array<SlotState, kRenderSnapshotSlots> m_slots{};
    std::deque<Queued> m_queue;
    bool m_stop = false;
    std::optional<glm::uvec2> m_pendingResize;
    bool m_pendingSettings = false;

    u32 m_nextSlot = 0; // game thread only
    Stats m_stats;      // guarded by m_mutex
};

} // namespace ox
