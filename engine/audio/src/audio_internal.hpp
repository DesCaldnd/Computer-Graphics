#pragma once

#include <oxwald/audio/audio_engine.hpp>

#include <miniaudio.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ox::audio::detail {

// Every custom node is this shell: ma_node_base must be the first member, `user` points to C++ state.
struct NodeShell {
    ma_node_base base;
    void* user = nullptr;
};

struct EffectSlot {
    NodeShell node{};
    bool nodeInit = false;
    std::unique_ptr<IAudioEffect> effect;
};

struct BusInternals;

struct DuckRule {
    const BusInternals* sidechain = nullptr;
    f32 threshold = 0.01f;
    f32 duckVolume = 0.3f;
    f32 attack = 0.05f;
    f32 release = 0.5f;
};

struct BusInternals {
    ma_sound_group group{};
    bool groupInit = false;
    NodeShell tail{};
    bool tailInit = false;
    std::vector<std::unique_ptr<EffectSlot>> effects;

    u32 channels = 2;
    u32 sampleRate = 48000;

    // game thread → audio thread
    std::atomic<f32> targetGain{1.f};
    // audio thread → game thread
    std::atomic<f32> peak{0.f};
    std::atomic<f32> rms{0.f};
    std::atomic<f32> duck{1.f};

    // audio-thread state
    f32 currentGain = 1.f;
    f32 currentDuck = 1.f;

    std::mutex duckMutex; // audio thread only try_locks
    std::vector<DuckRule> duckRules;
};

// Per-voice low-pass (distance absorption + occlusion) and occlusion gain.
struct VoiceFilterState {
    std::atomic<f32> cutoff{20000.f};
    std::atomic<f32> gain{1.f};
    f32 currentGain = 1.f;
    f32 currentCutoff = 20000.f;
    f32 s1[8]{};
    f32 s2[8]{};
    u32 channels = 2;
    u32 sampleRate = 48000;
};

struct SoundData {
    enum class Kind : u8 { File, Pcm, Sine, Noise } kind = Kind::Pcm;
    std::string path;
    LoadMode mode = LoadMode::Decode;
    std::vector<f32> pcm;
    u32 channels = 1;
    u32 sampleRate = 48000;
    f32 frequency = 440.f;
    f32 amplitude = 0.5f;
    NoiseType noise = NoiseType::White;
    i32 seed = 1;
    f32 duration = 0.f;
    std::unique_ptr<ma_sound> prototype; // keeps decoded file data resident in the resource manager
};

enum class VoiceState : u8 { Playing, Paused, Stopping };

struct Voice {
    u32 generation = 1;
    bool active = false;
    VoiceState state = VoiceState::Playing;
    SoundId soundId;
    PlayParams params;
    AudioBus* bus = nullptr;
    f32 occlusion = 0.f;
    f32 lastAudibility = 1.f;
    u64 sequence = 0; // play order, tie breaker for stealing (older first)

    ma_sound sound{};
    bool soundInit = false;
    NodeShell filterNode{};
    bool filterInit = false;
    VoiceFilterState filter;

    ma_waveform waveform{};
    bool hasWaveform = false;
    ma_noise noise{};
    bool hasNoise = false;
    ma_audio_buffer_ref bufferRef{};
    bool hasBufferRef = false;
};

struct SnapshotTransition {
    struct Entry {
        AudioBus* bus;
        f32 from, to;
        std::optional<bool> mute;
    };
    std::vector<Entry> entries;
    f32 duration = 0.f;
    f32 elapsed = 0.f;
};

struct EngineImpl {
    AudioEngineConfig config;
    ma_engine engine{};
    bool engineInit = false;

    std::unordered_map<u32, std::unique_ptr<SoundData>> sounds;
    u32 nextSoundId = 1;

    std::vector<std::unique_ptr<Voice>> voices;
    u64 voiceSequence = 0;

    std::vector<std::unique_ptr<AudioBus>> buses; // parents precede children
    AudioBus* masterBus = nullptr;
    std::vector<DuckingSettings> ducking;
    std::unordered_map<std::string, MixerSnapshot> snapshots;
    std::optional<SnapshotTransition> transition;

    ListenerState listener;
    IAudioOcclusionProvider* occlusionProvider = nullptr;

    // bus plumbing (audio_bus.cpp)
    AudioBus* createBus(const std::string& name, AudioBus* parent);
    void rewireBus(AudioBus& bus);
    void refreshBusGains();
    void rebuildDucking();
    [[nodiscard]] AudioBus* findBus(std::string_view name) const;
    [[nodiscard]] BusInternals& internals(AudioBus& bus) const { return *bus.m_internals; }

    // voices (audio_engine.cpp)
    Voice* resolve(SoundHandle h) const;
    void freeVoice(Voice& v);
    void applySpatial(Voice& v);
    f32 estimateAudibility(const PlayParams& params, const AudioBus* bus, f32 occlusion, f32 fade) const;
    void updateVoice(Voice& v, f32 dt);
};

ma_node_graph* nodeGraph(EngineImpl& e);
bool initEffectNode(EngineImpl& e, EffectSlot& slot, u32 channels);
bool initBusTailNode(EngineImpl& e, BusInternals& bus);
bool initVoiceFilterNode(EngineImpl& e, Voice& voice);

} // namespace ox::audio::detail
