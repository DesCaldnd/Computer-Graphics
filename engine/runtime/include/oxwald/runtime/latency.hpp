#pragma once

#include <oxwald/core/types.hpp>

#include <array>

// Engine-side latency limiter: the fallback for r.Reflex when the GPU/driver has no VK_NV_low_latency2 (AMD, Intel,
// Apple, Android, older NVIDIA drivers). Works with any renderer, see docs/dev/modules/runtime.md "Latency".
//
// JustInTime: every frame the engine measures how long the finished frames were queued behind the GPU / display
// (game thread waiting for a snapshot slot + render thread waiting for a frame slot + swapchain acquire). That
// queueing is pure latency: input sampled earlier than necessary. The limiter delays the start of the next frame
// (before input is sampled) by that amount minus a small safety margin, so the frame reaches the GPU just when it can
// start on it. A controller instead of a fixed delay: the queueing shrinks as the delay grows, the minimum over a
// short window is used so a single hitch does not make the delay overshoot, and the delay decays back to zero when
// the game becomes CPU bound (no queueing).
// Strict (Engine): wait until the GPU finished the previous frame before sampling input (no frame is ever queued;
// costs throughput because CPU and GPU no longer overlap).
namespace ox {

class LatencyLimiter {
public:
    struct Config {
        f64 targetQueueMs = 1.0; // queueing kept as headroom so the GPU never starves
        f64 gain = 0.5;          // fraction of the excess queueing removed per frame
        f64 decayMs = 0.25;      // minimum delay reduction per frame while nothing is queued (CPU bound)
        f64 maxDelayMs = 100.0;
        u32 window = 4;          // frames whose minimum queueing is used
    };

    LatencyLimiter() = default;
    explicit LatencyLimiter(const Config& config) : m_config(config) {}

    // Game thread, once per frame: queueMs = time the previous frame spent queued (see above). Returns the delay to
    // sleep before this frame samples input.
    f64 update(f64 queueMs);
    void reset();
    [[nodiscard]] f64 delayMs() const { return m_delayMs; }
    [[nodiscard]] const Config& config() const { return m_config; }

private:
    static constexpr u32 kMaxWindow = 16;
    Config m_config;
    std::array<f64, kMaxWindow> m_history{};
    u32 m_count = 0;
    u32 m_pos = 0;
    f64 m_delayMs = 0.0;
};

} // namespace ox
