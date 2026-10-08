#include <oxwald/audio/audio_effects.hpp>

#include <algorithm>
#include <cmath>

namespace ox::audio {

namespace {
constexpr f32 kPi = 3.14159265358979f;
}

void Biquad::setLowPass(f32 sampleRate, f32 cutoff, f32 q) {
    cutoff = std::clamp(cutoff, 10.f, sampleRate * 0.49f);
    const f32 w0 = 2.f * kPi * cutoff / sampleRate;
    const f32 cw = std::cos(w0), alpha = std::sin(w0) / (2.f * q);
    const f32 a0 = 1.f + alpha;
    b0 = (1.f - cw) * 0.5f / a0;
    b1 = (1.f - cw) / a0;
    b2 = b0;
    a1 = -2.f * cw / a0;
    a2 = (1.f - alpha) / a0;
}

void Biquad::setHighPass(f32 sampleRate, f32 cutoff, f32 q) {
    cutoff = std::clamp(cutoff, 10.f, sampleRate * 0.49f);
    const f32 w0 = 2.f * kPi * cutoff / sampleRate;
    const f32 cw = std::cos(w0), alpha = std::sin(w0) / (2.f * q);
    const f32 a0 = 1.f + alpha;
    b0 = (1.f + cw) * 0.5f / a0;
    b1 = -(1.f + cw) / a0;
    b2 = b0;
    a1 = -2.f * cw / a0;
    a2 = (1.f - alpha) / a0;
}

void Biquad::processInterleaved(f32* frames, u32 frameCount, u32 channels) {
    channels = std::min<u32>(channels, static_cast<u32>(z.size()));
    for (u32 c = 0; c < channels; ++c) {
        f32 z1 = z[c][0], z2 = z[c][1];
        for (u32 i = 0; i < frameCount; ++i) {
            f32& s = frames[i * channels + c];
            // Transposed direct form II.
            const f32 y = b0 * s + z1;
            z1 = b1 * s - a1 * y + z2;
            z2 = b2 * s - a2 * y;
            s = y;
        }
        z[c][0] = z1;
        z[c][1] = z2;
    }
}

void FilterEffect::prepare(u32 sampleRate, u32 channels) {
    m_sampleRate = sampleRate;
    m_channels = channels;
    m_appliedCutoff = -1.f;
    m_biquad.clear();
}

void FilterEffect::process(f32* frames, u32 frameCount) {
    const f32 cutoff = m_cutoff.load(std::memory_order_relaxed);
    const f32 q = m_q.load(std::memory_order_relaxed);
    if (cutoff != m_appliedCutoff || q != m_appliedQ) {
        if (m_kind == Kind::LowPass) {
            m_biquad.setLowPass(static_cast<f32>(m_sampleRate), cutoff, q);
        } else {
            m_biquad.setHighPass(static_cast<f32>(m_sampleRate), cutoff, q);
        }
        m_appliedCutoff = cutoff;
        m_appliedQ = q;
    }
    m_biquad.processInterleaved(frames, frameCount, m_channels);
}

void DelayEffect::prepare(u32 sampleRate, u32 channels) {
    m_sampleRate = sampleRate;
    m_channels = channels;
    m_capacityFrames = static_cast<u32>(kMaxDelaySeconds * static_cast<f32>(sampleRate)) + 1;
    m_buffer.assign(static_cast<usize>(m_capacityFrames) * channels, 0.f);
    m_writePos = 0;
}

void DelayEffect::reset() {
    std::fill(m_buffer.begin(), m_buffer.end(), 0.f);
    m_writePos = 0;
}

void DelayEffect::process(f32* frames, u32 frameCount) {
    if (m_capacityFrames == 0) {
        return;
    }
    const u32 delayFrames = std::clamp<u32>(
        static_cast<u32>(m_delay.load(std::memory_order_relaxed) * static_cast<f32>(m_sampleRate)), 1,
        m_capacityFrames - 1);
    const f32 fb = std::clamp(m_feedback.load(std::memory_order_relaxed), 0.f, 0.98f);
    const f32 wet = m_wet.load(std::memory_order_relaxed), dry = m_dry.load(std::memory_order_relaxed);
    for (u32 i = 0; i < frameCount; ++i) {
        const u32 readPos = (m_writePos + m_capacityFrames - delayFrames) % m_capacityFrames;
        for (u32 c = 0; c < m_channels; ++c) {
            f32& s = frames[i * m_channels + c];
            const f32 delayed = m_buffer[readPos * m_channels + c];
            m_buffer[m_writePos * m_channels + c] = s + delayed * fb;
            s = s * dry + delayed * wet;
        }
        m_writePos = (m_writePos + 1) % m_capacityFrames;
    }
}

namespace {
// Freeverb tunings at 44.1 kHz.
constexpr std::array<u32, 8> kCombTuning{1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
constexpr std::array<u32, 4> kAllPassTuning{556, 441, 341, 225};
constexpr u32 kStereoSpread = 23;
constexpr f32 kFixedGain = 0.015f;
constexpr f32 kScaleRoom = 0.28f, kOffsetRoom = 0.7f, kScaleDamp = 0.4f;
} // namespace

ReverbEffect::ReverbEffect(const Params& p)
    : m_roomSize(p.roomSize), m_damping(p.damping), m_wet(p.wet), m_dry(p.dry), m_width(p.width) {}

void ReverbEffect::setParams(const Params& p) {
    m_roomSize.store(p.roomSize);
    m_damping.store(p.damping);
    m_wet.store(p.wet);
    m_dry.store(p.dry);
    m_width.store(p.width);
}

ReverbEffect::Params ReverbEffect::params() const {
    return {m_roomSize.load(), m_damping.load(), m_wet.load(), m_dry.load(), m_width.load()};
}

void ReverbEffect::prepare(u32 sampleRate, u32 channels) {
    m_channels = channels;
    const f32 scale = static_cast<f32>(sampleRate) / 44100.f;
    for (u32 side = 0; side < 2; ++side) {
        const u32 spread = side * kStereoSpread;
        for (usize i = 0; i < 8; ++i) {
            m_combs[side][i].buf.assign(static_cast<usize>(static_cast<f32>(kCombTuning[i] + spread) * scale), 0.f);
            m_combs[side][i].pos = 0;
            m_combs[side][i].store = 0.f;
        }
        for (usize i = 0; i < 4; ++i) {
            m_allPasses[side][i].buf.assign(static_cast<usize>(static_cast<f32>(kAllPassTuning[i] + spread) * scale),
                                            0.f);
            m_allPasses[side][i].pos = 0;
        }
    }
}

void ReverbEffect::reset() {
    for (auto& side : m_combs) {
        for (auto& c : side) {
            std::fill(c.buf.begin(), c.buf.end(), 0.f);
            c.store = 0.f;
        }
    }
    for (auto& side : m_allPasses) {
        for (auto& a : side) {
            std::fill(a.buf.begin(), a.buf.end(), 0.f);
        }
    }
}

void ReverbEffect::process(f32* frames, u32 frameCount) {
    if (m_combs[0][0].buf.empty()) {
        return;
    }
    const f32 feedback = std::clamp(m_roomSize.load(), 0.f, 1.f) * kScaleRoom + kOffsetRoom;
    const f32 damp1 = std::clamp(m_damping.load(), 0.f, 1.f) * kScaleDamp, damp2 = 1.f - damp1;
    const f32 wet = m_wet.load() * 3.f, dry = m_dry.load(), width = std::clamp(m_width.load(), 0.f, 1.f);
    const f32 wet1 = wet * (width * 0.5f + 0.5f), wet2 = wet * ((1.f - width) * 0.5f);
    const u32 ch = m_channels;

    for (u32 i = 0; i < frameCount; ++i) {
        f32* f = frames + static_cast<usize>(i) * ch;
        const f32 inL = f[0], inR = ch > 1 ? f[1] : f[0];
        const f32 input = (inL + inR) * kFixedGain;
        f32 out[2] = {0.f, 0.f};
        for (u32 side = 0; side < 2; ++side) {
            for (auto& c : m_combs[side]) {
                const f32 o = c.buf[c.pos];
                c.store = o * damp2 + c.store * damp1;
                c.buf[c.pos] = input + c.store * feedback;
                if (++c.pos >= c.buf.size()) {
                    c.pos = 0;
                }
                out[side] += o;
            }
            for (auto& a : m_allPasses[side]) {
                const f32 b = a.buf[a.pos];
                const f32 o = -out[side] + b;
                a.buf[a.pos] = out[side] + b * 0.5f;
                if (++a.pos >= a.buf.size()) {
                    a.pos = 0;
                }
                out[side] = o;
            }
        }
        if (ch == 1) {
            f[0] = inL * dry + (out[0] + out[1]) * 0.5f * wet1;
        } else {
            f[0] = inL * dry + out[0] * wet1 + out[1] * wet2;
            f[1] = inR * dry + out[1] * wet1 + out[0] * wet2;
        }
    }
}

} // namespace ox::audio
