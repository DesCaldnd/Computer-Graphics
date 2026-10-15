#include <oxwald/core/assert.hpp>
#include <oxwald/core/profile.hpp>
#include <oxwald/core/time.hpp>
#include <oxwald/runtime/render_pipeline.hpp>

namespace ox {

RenderPipeline::~RenderPipeline() { stop(); }

void RenderPipeline::start(IRenderer& renderer, Mode mode) {
    OX_ASSERT(m_renderer == nullptr, "render pipeline already running");
    m_renderer = &renderer;
    m_mode = mode;
    m_stop = false;
    m_slots.fill(SlotState::Free);
    m_queue.clear();
    m_nextSlot = 0;
    if (mode == Mode::Threaded) {
        m_thread = std::thread([this] { renderLoop(); });
    }
}

void RenderPipeline::stop() {
    if (!m_renderer) return;
    if (m_thread.joinable()) {
        {
            std::lock_guard lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
        m_thread.join();
    }
    m_renderer = nullptr;
}

void RenderPipeline::submit(const World& world, FrameContext ctx) {
    OX_PROFILE_ZONE();
    OX_ASSERT(m_renderer != nullptr, "render pipeline not started");
    ctx.slot = m_nextSlot;
    m_nextSlot = (m_nextSlot + 1) % kRenderSnapshotSlots;

    if (m_mode == Mode::SingleThreaded) {
        Stopwatch sw(true);
        m_renderer->extract(world, ctx);
        const f64 extractMs = sw.lap() * 1000.0;
        applyPendingCommands();
        m_renderer->render(ctx);
        const f64 renderMs = sw.lap() * 1000.0;
        std::lock_guard lock(m_mutex);
        m_stats.framesSubmitted++;
        m_stats.framesRendered++;
        m_stats.lastExtractMs = extractMs;
        m_stats.lastRenderMs = renderMs;
        m_stats.lastWaitMs = 0.0;
        return;
    }

    Stopwatch wait(true);
    {
        OX_PROFILE_ZONE_N("WaitForSnapshotSlot");
        std::unique_lock lock(m_mutex);
        m_cv.wait(lock, [&] { return m_slots[ctx.slot] == SlotState::Free; });
        m_slots[ctx.slot] = SlotState::Extracting;
        m_stats.lastWaitMs = wait.elapsedMs();
    }
    Stopwatch sw(true);
    m_renderer->extract(world, ctx);
    const f64 extractMs = sw.elapsedMs();
    {
        std::lock_guard lock(m_mutex);
        m_slots[ctx.slot] = SlotState::Ready;
        m_queue.push_back(Queued{ctx, std::chrono::steady_clock::now()});
        m_stats.framesSubmitted++;
        m_stats.lastExtractMs = extractMs;
    }
    m_cv.notify_all();
}

void RenderPipeline::flush() {
    if (!m_renderer || m_mode == Mode::SingleThreaded) return;
    OX_PROFILE_ZONE();
    std::unique_lock lock(m_mutex);
    m_cv.wait(lock, [&] { return m_stats.framesRendered == m_stats.framesSubmitted; });
}

std::chrono::steady_clock::time_point RenderPipeline::waitUntilTaken(std::chrono::milliseconds timeout) {
    const auto now = std::chrono::steady_clock::now();
    if (!m_renderer || m_mode == Mode::SingleThreaded) return now;
    OX_PROFILE_ZONE();
    std::unique_lock lock(m_mutex);
    if (!m_cv.wait_for(lock, timeout, [&] { return m_queue.empty(); })) return std::chrono::steady_clock::now();
    return m_lastTakenAt == std::chrono::steady_clock::time_point{} ? now : m_lastTakenAt;
}

void RenderPipeline::requestResize(glm::uvec2 size) {
    std::lock_guard lock(m_mutex);
    m_pendingResize = size;
}

void RenderPipeline::requestSettingsChanged() {
    std::lock_guard lock(m_mutex);
    m_pendingSettings = true;
}

RenderPipeline::Stats RenderPipeline::stats() const {
    std::lock_guard lock(m_mutex);
    return m_stats;
}

void RenderPipeline::applyPendingCommands() {
    std::optional<glm::uvec2> resize;
    bool settings = false;
    {
        std::lock_guard lock(m_mutex);
        resize = std::exchange(m_pendingResize, std::nullopt);
        settings = std::exchange(m_pendingSettings, false);
    }
    if (resize) m_renderer->resize(*resize);
    if (settings) m_renderer->settingsChanged();
}

void RenderPipeline::renderLoop() {
    OX_PROFILE_THREAD_NAME("Render");
    for (;;) {
        Queued item;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [&] { return m_stop || !m_queue.empty(); });
            if (m_queue.empty()) return; // stop requested and drained
            item = m_queue.front();
            m_queue.pop_front();
            m_slots[item.ctx.slot] = SlotState::Rendering;
            m_lastTakenAt = std::chrono::steady_clock::now();
            m_stats.lastQueuedMs = std::chrono::duration<f64, std::milli>(m_lastTakenAt - item.readyAt).count();
        }
        m_cv.notify_all(); // waitUntilTaken
        applyPendingCommands();
        Stopwatch sw(true);
        {
            OX_PROFILE_ZONE_N("RenderFrame");
            m_renderer->render(item.ctx);
        }
        const f64 ms = sw.elapsedMs();
        {
            std::lock_guard lock(m_mutex);
            m_slots[item.ctx.slot] = SlotState::Free;
            m_stats.framesRendered++;
            m_stats.lastRenderMs = ms;
        }
        OX_PROFILE_FRAME_N("Render");
        m_cv.notify_all();
    }
}

} // namespace ox
