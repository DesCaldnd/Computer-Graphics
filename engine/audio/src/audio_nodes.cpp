#include "audio_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

namespace ox::audio::detail {

namespace {

constexpr f32 kTwoPi = 6.28318530718f;

// Copies the (single) input bus to the output, zero-filling when the input delivered fewer frames.
u32 passThrough(ma_node* node, const float** in, ma_uint32* inCount, float** out, ma_uint32* outCount) {
    const u32 channels = ma_node_get_output_channels(node, 0);
    const u32 frames = *outCount;
    const u32 avail = (in != nullptr && in[0] != nullptr && inCount != nullptr) ? std::min<u32>(*inCount, frames) : 0;
    if (avail > 0) {
        std::memcpy(out[0], in[0], sizeof(f32) * avail * channels);
    }
    if (avail < frames) {
        std::memset(out[0] + static_cast<usize>(avail) * channels, 0, sizeof(f32) * (frames - avail) * channels);
    }
    if (inCount != nullptr) {
        *inCount = avail;
    }
    return channels;
}

void effectProcess(ma_node* node, const float** in, ma_uint32* inCount, float** out, ma_uint32* outCount) {
    passThrough(node, in, inCount, out, outCount);
    auto* effect = static_cast<IAudioEffect*>(reinterpret_cast<NodeShell*>(node)->user);
    if (effect != nullptr && !effect->bypassed()) {
        effect->process(out[0], *outCount);
    }
}

void busTailProcess(ma_node* node, const float** in, ma_uint32* inCount, float** out, ma_uint32* outCount) {
    const u32 channels = passThrough(node, in, inCount, out, outCount);
    auto& bus = *static_cast<BusInternals*>(reinterpret_cast<NodeShell*>(node)->user);
    const u32 frames = *outCount;
    if (frames == 0) {
        return;
    }
    const f32 blockSeconds = static_cast<f32>(frames) / static_cast<f32>(bus.sampleRate);

    // Sidechain ducking: the most demanding active rule wins.
    f32 desiredDuck = 1.f;
    f32 attack = 0.05f, release = 0.5f;
    if (bus.duckMutex.try_lock()) {
        for (const DuckRule& r : bus.duckRules) {
            attack = r.attack;
            release = r.release;
            if (r.sidechain != nullptr && r.sidechain->rms.load(std::memory_order_relaxed) > r.threshold) {
                desiredDuck = std::min(desiredDuck, r.duckVolume);
            }
        }
        bus.duckMutex.unlock();
    } else {
        desiredDuck = bus.currentDuck;
    }
    const f32 tau = desiredDuck < bus.currentDuck ? attack : release;
    const f32 k = 1.f - std::exp(-blockSeconds / std::max(tau, 1e-4f));
    const f32 newDuck = bus.currentDuck + (desiredDuck - bus.currentDuck) * k;

    const f32 g0 = bus.currentGain * bus.currentDuck;
    const f32 target = bus.targetGain.load(std::memory_order_relaxed);
    const f32 g1 = target * newDuck;
    f32 peak = 0.f;
    f64 sumSq = 0.0;
    f32* o = out[0];
    for (u32 i = 0; i < frames; ++i) {
        const f32 g = g0 + (g1 - g0) * (static_cast<f32>(i + 1) / static_cast<f32>(frames));
        for (u32 c = 0; c < channels; ++c) {
            f32& s = o[i * channels + c];
            s *= g;
            peak = std::max(peak, std::abs(s));
            sumSq += static_cast<f64>(s) * s;
        }
    }
    bus.currentGain = target;
    bus.currentDuck = newDuck;
    bus.peak.store(peak, std::memory_order_relaxed);
    bus.rms.store(static_cast<f32>(std::sqrt(sumSq / (static_cast<f64>(frames) * channels))), std::memory_order_relaxed);
    bus.duck.store(newDuck, std::memory_order_relaxed);
}

void voiceFilterProcess(ma_node* node, const float** in, ma_uint32* inCount, float** out, ma_uint32* outCount) {
    const u32 channels = std::min<u32>(passThrough(node, in, inCount, out, outCount), 8);
    auto& st = *static_cast<VoiceFilterState*>(reinterpret_cast<NodeShell*>(node)->user);
    const u32 frames = *outCount;
    if (frames == 0) {
        return;
    }
    const f32 targetCutoff = st.cutoff.load(std::memory_order_relaxed);
    const f32 targetGain = st.gain.load(std::memory_order_relaxed);
    const f32 nyquist = 0.5f * static_cast<f32>(st.sampleRate);
    f32* o = out[0];

    const bool filtering = targetCutoff < nyquist * 0.8f || st.currentCutoff < nyquist * 0.8f;
    if (filtering) {
        // Two cascaded one-pole low-passes (12 dB/oct); coefficient interpolated over the block.
        auto coef = [&](f32 fc) { return 1.f - std::exp(-kTwoPi * std::min(fc, nyquist) / static_cast<f32>(st.sampleRate)); };
        const f32 a0 = coef(st.currentCutoff), a1 = coef(targetCutoff);
        for (u32 i = 0; i < frames; ++i) {
            const f32 a = a0 + (a1 - a0) * (static_cast<f32>(i + 1) / static_cast<f32>(frames));
            for (u32 c = 0; c < channels; ++c) {
                f32& s = o[i * channels + c];
                st.s1[c] += a * (s - st.s1[c]);
                st.s2[c] += a * (st.s1[c] - st.s2[c]);
                s = st.s2[c];
            }
        }
    } else {
        for (u32 i = 0; i < frames; ++i) {
            for (u32 c = 0; c < channels; ++c) {
                st.s1[c] = st.s2[c] = o[i * channels + c];
            }
        }
    }
    st.currentCutoff = targetCutoff;

    if (targetGain != 1.f || st.currentGain != 1.f) {
        const f32 g0 = st.currentGain;
        for (u32 i = 0; i < frames; ++i) {
            const f32 g = g0 + (targetGain - g0) * (static_cast<f32>(i + 1) / static_cast<f32>(frames));
            for (u32 c = 0; c < channels; ++c) {
                o[i * channels + c] *= g;
            }
        }
    }
    st.currentGain = targetGain;
}

ma_node_vtable gEffectVTable{effectProcess, nullptr, 1, 1, MA_NODE_FLAG_CONTINUOUS_PROCESSING};
ma_node_vtable gBusTailVTable{busTailProcess, nullptr, 1, 1, MA_NODE_FLAG_CONTINUOUS_PROCESSING};
ma_node_vtable gVoiceFilterVTable{voiceFilterProcess, nullptr, 1, 1, 0};

bool initShell(EngineImpl& e, NodeShell& shell, const ma_node_vtable* vtable, u32 channels, void* user) {
    shell.user = user;
    ma_node_config cfg = ma_node_config_init();
    cfg.vtable = vtable;
    cfg.pInputChannels = &channels;
    cfg.pOutputChannels = &channels;
    return ma_node_init(nodeGraph(e), &cfg, nullptr, &shell.base) == MA_SUCCESS;
}

} // namespace

ma_node_graph* nodeGraph(EngineImpl& e) { return ma_engine_get_node_graph(&e.engine); }

bool initEffectNode(EngineImpl& e, EffectSlot& slot, u32 channels) {
    slot.effect->prepare(ma_engine_get_sample_rate(&e.engine), channels);
    slot.nodeInit = initShell(e, slot.node, &gEffectVTable, channels, slot.effect.get());
    return slot.nodeInit;
}

bool initBusTailNode(EngineImpl& e, BusInternals& bus) {
    bus.tailInit = initShell(e, bus.tail, &gBusTailVTable, bus.channels, &bus);
    return bus.tailInit;
}

bool initVoiceFilterNode(EngineImpl& e, Voice& voice) {
    VoiceFilterState& st = voice.filter;
    st.cutoff.store(20000.f);
    st.gain.store(1.f);
    st.currentGain = 1.f;
    std::fill(std::begin(st.s1), std::end(st.s1), 0.f);
    std::fill(std::begin(st.s2), std::end(st.s2), 0.f);
    voice.filter.channels = ma_engine_get_channels(&e.engine);
    voice.filter.sampleRate = ma_engine_get_sample_rate(&e.engine);
    voice.filter.currentCutoff = voice.filter.cutoff.load();
    voice.filterInit = initShell(e, voice.filterNode, &gVoiceFilterVTable, voice.filter.channels, &voice.filter);
    return voice.filterInit;
}

} // namespace ox::audio::detail
