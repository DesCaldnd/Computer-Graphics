#pragma once

// Low latency support of the runtime renderer (included by runtime_renderer.cpp only):
//  - NVIDIA Reflex through rhi::Swapchain (VK_NV_low_latency2): mode, sleep, markers, timing reports;
//  - GPU completion watcher for the engine's fallback limiter: per frame, how long the frame's GPU work waited in the
//    queue behind the previous frame (submit -> previous frame's completion), measured on the CPU with a thread
//    that waits on the graphics timeline semaphore, and the engine's own latency measure (simulation start -> GPU
//    done, LatencyStats::engineLatencyMs). Vendor independent.

#include <oxwald/core/log.hpp>
#include <oxwald/rhi/device.hpp>
#include <oxwald/rhi/swapchain.hpp>
#include <oxwald/runtime/renderer.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace ox::render {

class LatencyTracker {
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    LatencyTracker() = default;
    LatencyTracker(const LatencyTracker&) = delete;
    LatencyTracker& operator=(const LatencyTracker&) = delete;
    ~LatencyTracker() { stop(); }

    void start(rhi::Device& device, rhi::Swapchain* swapchain) {
        m_device = &device;
        m_swapchain = swapchain;
        m_semaphore = device.timelineSemaphore(rhi::QueueType::Graphics);
        m_stop = false;
        m_thread = std::thread([this] { watchLoop(); });
    }

    void stop() {
        if (!m_thread.joinable()) return;
        {
            std::lock_guard lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
        m_thread.join();
    }

    // ---- game thread ----
    f64 sleep(u64 frameIndex, i32 mode) {
        (void)frameIndex;
        m_mode.store(mode, std::memory_order_relaxed);
        if (!m_swapchain || !m_swapchain->lowLatencySupported()) return -1.0;
        rhi::LowLatencyMode m;
        m.enabled = mode >= 1;
        m.boost = mode >= 2;
        if (m.enabled != m_applied.enabled || m.boost != m_applied.boost) {
            m_swapchain->setLowLatencyMode(m);
            m_applied = m;
        }
        if (!m.enabled) return -1.0;
        const f64 slept = m_swapchain->latencySleep();
        m_lastSleepMs.store(std::max(slept, 0.0), std::memory_order_relaxed);
        return slept;
    }

    void marker(u64 frameIndex, LatencyMarker marker) {
        if (marker == LatencyMarker::SimulationStart) {
            std::lock_guard lock(m_mutex);
            m_simStart[frameIndex % kFrames] = {frameIndex, std::chrono::steady_clock::now()};
        }
        if (!m_swapchain) return;
        const rhi::LatencyMarker m = marker == LatencyMarker::SimulationStart ? rhi::LatencyMarker::SimulationStart
                                     : marker == LatencyMarker::SimulationEnd ? rhi::LatencyMarker::SimulationEnd
                                                                              : rhi::LatencyMarker::InputSample;
        m_swapchain->setLatencyMarker(frameIndex + 1, m);
    }

    // Blocks until the GPU finished frame `frameIndex` (false: unknown frame / no device).
    bool waitForFrame(u64 frameIndex) {
        u64 value = 0;
        {
            std::lock_guard lock(m_mutex);
            const FrameRecord& r = m_frames[frameIndex % kFrames];
            if (r.frame != frameIndex || r.value == 0) return false;
            value = r.value;
        }
        VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wi.semaphoreCount = 1;
        wi.pSemaphores = &m_semaphore;
        wi.pValues = &value;
        return vkWaitSemaphores(m_device->vkDevice(), &wi, 1'000'000'000ull) == VK_SUCCESS;
    }

    // ---- render thread ----
    void renderSubmitStart(u64 frameIndex) {
        if (m_swapchain) {
            m_swapchain->setPresentId(frameIndex + 1);
            m_swapchain->setLatencyMarker(frameIndex + 1, rhi::LatencyMarker::RenderSubmitStart);
        }
    }
    void renderSubmitEnd(u64 frameIndex) {
        if (m_swapchain) m_swapchain->setLatencyMarker(frameIndex + 1, rhi::LatencyMarker::RenderSubmitEnd);
    }
    // After the frame's last submit (Device::endFrame): the watcher measures its GPU queueing.
    void frameSubmitted(u64 frameIndex, u64 timelineValue, TimePoint submitTime) {
        if (timelineValue == 0) return;
        {
            std::lock_guard lock(m_mutex);
            m_frames[frameIndex % kFrames] = {frameIndex, timelineValue, submitTime, {}};
            m_pending.push_back(frameIndex);
        }
        m_cv.notify_one();
    }

    // Render thread, once per frame: refreshes the Reflex report averages every few frames.
    void fillStats(LatencyStats& out, u64 frameIndex) {
        out.mode = m_mode.load(std::memory_order_relaxed);
        out.reflexSupported = m_swapchain && m_swapchain->lowLatencySupported();
        out.reflexBehindFrameGeneration = m_swapchain && m_swapchain->lowLatencyCapable() && m_swapchain->activeProxy();
        out.reflexActive = out.reflexSupported && out.mode >= 1;
        out.sleepMs = out.reflexActive ? m_lastSleepMs.load(std::memory_order_relaxed) : 0.0;
        if (out.reflexSupported && (frameIndex % 8) == 0) {
            const std::vector<rhi::LatencyFrameTimings> t = m_swapchain->latencyTimings();
            LatencyStats avg;
            for (const rhi::LatencyFrameTimings& f : t) {
                if (!f.complete()) continue;
                ++avg.reports;
                avg.pcLatencyMs += f.pcLatencyMs();
                avg.gameToRenderMs += f.gameToRenderMs();
                avg.renderQueueMs += f.renderQueueMs();
                avg.gpuRenderMs += f.gpuRenderMs();
            }
            if (avg.reports) {
                const f64 n = f64(avg.reports);
                m_reports = avg.reports;
                m_pcLatencyMs = avg.pcLatencyMs / n;
                m_gameToRenderMs = avg.gameToRenderMs / n;
                m_renderQueueMs = avg.renderQueueMs / n;
                m_gpuRenderMs = avg.gpuRenderMs / n;
            }
        }
        out.reports = m_reports;
        out.pcLatencyMs = m_pcLatencyMs;
        out.gameToRenderMs = m_gameToRenderMs;
        out.renderQueueMs = m_renderQueueMs;
        out.gpuRenderMs = m_gpuRenderMs;
        out.engineLatencyMs = m_engineLatencyMs.load(std::memory_order_relaxed);
    }

    // Latest GPU queueing (ms) of a frame behind its predecessor: previous frame done - this frame submitted. Signed:
    // < 0 when the GPU sat idle before this frame (starved; the fallback limiter must back off).
    [[nodiscard]] f64 gpuQueueMs() const { return m_gpuQueueMs.load(std::memory_order_relaxed); }

private:
    static constexpr u32 kFrames = 16;
    struct FrameRecord {
        u64 frame = ~0ull;
        u64 value = 0;
        TimePoint submitted{};
        TimePoint completed{};
    };
    struct SimStart {
        u64 frame = ~0ull;
        TimePoint time{};
    };

    void watchLoop() {
        for (;;) {
            u64 frame = 0, value = 0;
            {
                std::unique_lock lock(m_mutex);
                m_cv.wait(lock, [&] { return m_stop || !m_pending.empty(); });
                if (m_stop) return;
                frame = m_pending.front();
                m_pending.pop_front();
                const FrameRecord& r = m_frames[frame % kFrames];
                if (r.frame != frame) continue;
                value = r.value;
            }
            VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            wi.semaphoreCount = 1;
            wi.pSemaphores = &m_semaphore;
            wi.pValues = &value;
            VkResult res = VK_TIMEOUT;
            while (res == VK_TIMEOUT) {
                res = vkWaitSemaphores(m_device->vkDevice(), &wi, 50'000'000ull);
                std::lock_guard lock(m_mutex);
                if (m_stop) return;
            }
            const TimePoint done = std::chrono::steady_clock::now();
            std::lock_guard lock(m_mutex);
            FrameRecord& r = m_frames[frame % kFrames];
            if (r.frame != frame) continue;
            r.completed = done;
            if (const SimStart& s = m_simStart[frame % kFrames]; s.frame == frame) {
                // Exponential average over ~16 frames (stats are printed per second).
                const f64 l = std::chrono::duration<f64, std::milli>(done - s.time).count();
                const f64 prevAvg = m_engineLatencyMs.load(std::memory_order_relaxed);
                m_engineLatencyMs.store(prevAvg > 0.0 ? prevAvg + (l - prevAvg) / 16.0 : l, std::memory_order_relaxed);
            }
            const FrameRecord& prev = m_frames[(frame + kFrames - 1) % kFrames];
            if (frame > 0 && prev.frame == frame - 1 && prev.completed != TimePoint{}) {
                const f64 q = std::chrono::duration<f64, std::milli>(prev.completed - r.submitted).count();
                m_gpuQueueMs.store(std::max(q, -50.0), std::memory_order_relaxed); // < 0: the GPU sat idle
            }
        }
    }

    rhi::Device* m_device = nullptr;
    rhi::Swapchain* m_swapchain = nullptr;
    VkSemaphore m_semaphore = VK_NULL_HANDLE;
    rhi::LowLatencyMode m_applied;             // game thread
    std::atomic<i32> m_mode{0};
    std::atomic<f64> m_lastSleepMs{0.0};
    std::atomic<f64> m_gpuQueueMs{0.0};
    std::atomic<f64> m_engineLatencyMs{0.0};
    // Reflex report averages (render thread)
    u32 m_reports = 0;
    f64 m_pcLatencyMs = 0.0, m_gameToRenderMs = 0.0, m_renderQueueMs = 0.0, m_gpuRenderMs = 0.0;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::array<FrameRecord, kFrames> m_frames{};
    std::array<SimStart, kFrames> m_simStart{}; // game thread SimulationStart markers
    std::deque<u64> m_pending;
    bool m_stop = false;
    std::thread m_thread;
};

} // namespace ox::render
