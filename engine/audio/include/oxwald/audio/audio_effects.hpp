#pragma once

#include <oxwald/core/types.hpp>

#include <array>
#include <atomic>
#include <vector>

namespace ox::audio {

// DSP effect working in place on interleaved f32 frames. Runs on the audio thread (or inside
// AudioEngine::render in offline mode); parameters are atomics so the game thread may change them live.
// Effects are usable standalone (prepare + process) — the mixer wraps each one in a miniaudio node.
class IAudioEffect {
public:
    virtual ~IAudioEffect() = default;
    virtual void prepare(u32 sampleRate, u32 channels) = 0;
    virtual void process(f32* frames, u32 frameCount) = 0;
    virtual void reset() {}
    [[nodiscard]] virtual const char* typeName() const = 0;

    void setBypass(bool bypass) { m_bypass.store(bypass, std::memory_order_relaxed); }
    [[nodiscard]] bool bypassed() const { return m_bypass.load(std::memory_order_relaxed); }

private:
    std::atomic<bool> m_bypass{false};
};

// RBJ-cookbook biquad, one state pair per channel (max 8 channels).
struct Biquad {
    f32 b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    std::array<std::array<f32, 2>, 8> z{};
    void setLowPass(f32 sampleRate, f32 cutoff, f32 q);
    void setHighPass(f32 sampleRate, f32 cutoff, f32 q);
    void processInterleaved(f32* frames, u32 frameCount, u32 channels);
    void clear() { z = {}; }
};

class FilterEffect : public IAudioEffect {
public:
    enum class Kind : u8 { LowPass, HighPass };
    FilterEffect(Kind kind, f32 cutoffHz, f32 q = 0.7071f) : m_kind(kind), m_cutoff(cutoffHz), m_q(q) {}
    void setCutoff(f32 hz) { m_cutoff.store(hz, std::memory_order_relaxed); }
    void setQ(f32 q) { m_q.store(q, std::memory_order_relaxed); }
    [[nodiscard]] f32 cutoff() const { return m_cutoff.load(std::memory_order_relaxed); }

    void prepare(u32 sampleRate, u32 channels) override;
    void process(f32* frames, u32 frameCount) override;
    void reset() override { m_biquad.clear(); }
    [[nodiscard]] const char* typeName() const override { return m_kind == Kind::LowPass ? "LowPass" : "HighPass"; }

private:
    Kind m_kind;
    std::atomic<f32> m_cutoff;
    std::atomic<f32> m_q;
    f32 m_appliedCutoff = -1.f, m_appliedQ = -1.f;
    u32 m_sampleRate = 48000, m_channels = 2;
    Biquad m_biquad;
};

class LowPassEffect final : public FilterEffect {
public:
    explicit LowPassEffect(f32 cutoffHz = 5000.f, f32 q = 0.7071f) : FilterEffect(Kind::LowPass, cutoffHz, q) {}
};

class HighPassEffect final : public FilterEffect {
public:
    explicit HighPassEffect(f32 cutoffHz = 200.f, f32 q = 0.7071f) : FilterEffect(Kind::HighPass, cutoffHz, q) {}
};

// Feedback delay / echo.
class DelayEffect final : public IAudioEffect {
public:
    explicit DelayEffect(f32 delaySeconds = 0.25f, f32 feedback = 0.4f, f32 wet = 0.35f, f32 dry = 1.f)
        : m_delay(delaySeconds), m_feedback(feedback), m_wet(wet), m_dry(dry) {}
    static constexpr f32 kMaxDelaySeconds = 2.f;
    void setDelay(f32 s) { m_delay.store(s, std::memory_order_relaxed); }
    void setFeedback(f32 f) { m_feedback.store(f, std::memory_order_relaxed); }
    void setMix(f32 wet, f32 dry) { m_wet.store(wet); m_dry.store(dry); }

    void prepare(u32 sampleRate, u32 channels) override;
    void process(f32* frames, u32 frameCount) override;
    void reset() override;
    [[nodiscard]] const char* typeName() const override { return "Delay"; }

private:
    std::atomic<f32> m_delay, m_feedback, m_wet, m_dry;
    u32 m_sampleRate = 48000, m_channels = 2;
    std::vector<f32> m_buffer; // interleaved ring
    u32 m_writePos = 0, m_capacityFrames = 0;
};

// Freeverb (Schroeder/Moorer: 8 parallel damped combs + 4 series all-passes per channel).
class ReverbEffect final : public IAudioEffect {
public:
    struct Params {
        f32 roomSize = 0.7f; // 0..1
        f32 damping = 0.5f;  // 0..1
        f32 wet = 0.3f;
        f32 dry = 1.f;
        f32 width = 1.f;
    };
    ReverbEffect() : ReverbEffect(Params{}) {}
    explicit ReverbEffect(const Params& p);
    void setParams(const Params& p);
    [[nodiscard]] Params params() const;

    void prepare(u32 sampleRate, u32 channels) override;
    void process(f32* frames, u32 frameCount) override;
    void reset() override;
    [[nodiscard]] const char* typeName() const override { return "Reverb"; }

private:
    struct Comb {
        std::vector<f32> buf;
        u32 pos = 0;
        f32 store = 0;
    };
    struct AllPass {
        std::vector<f32> buf;
        u32 pos = 0;
    };
    std::atomic<f32> m_roomSize, m_damping, m_wet, m_dry, m_width;
    u32 m_channels = 2;
    std::array<std::array<Comb, 8>, 2> m_combs;
    std::array<std::array<AllPass, 4>, 2> m_allPasses;
};

} // namespace ox::audio
