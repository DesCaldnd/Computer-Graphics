#pragma once

#include <oxwald/audio/audio_effects.hpp>
#include <oxwald/core/types.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ox::audio {

namespace detail {
struct BusInternals;
struct EngineImpl;
} // namespace detail

// A mixer bus: sounds and child buses sum into it, then pass its effect chain and its fader/meter
// before flowing into the parent bus (Master → device). Owned by the AudioEngine.
class AudioBus {
public:
    AudioBus(const AudioBus&) = delete;
    AudioBus& operator=(const AudioBus&) = delete;
    ~AudioBus();

    [[nodiscard]] const std::string& name() const { return m_name; }
    [[nodiscard]] AudioBus* parent() const { return m_parent; }
    [[nodiscard]] const std::vector<AudioBus*>& children() const { return m_children; }

    void setVolume(f32 linear);
    void setVolumeDb(f32 db);
    [[nodiscard]] f32 volume() const { return m_volume; }
    void setMuted(bool muted);
    [[nodiscard]] bool muted() const { return m_muted; }
    // Solo: when any bus is soloed, buses that are neither soloed nor an ancestor/descendant of a soloed bus mute.
    void setSolo(bool solo);
    [[nodiscard]] bool solo() const { return m_solo; }

    // Effects are processed in insertion order, each as its own miniaudio node.
    IAudioEffect* addEffect(std::unique_ptr<IAudioEffect> effect);
    template <class T, class... Args>
    T* addEffect(Args&&... args) {
        return static_cast<T*>(addEffect(std::make_unique<T>(std::forward<Args>(args)...)));
    }
    bool removeEffect(IAudioEffect* effect);
    void clearEffects();
    [[nodiscard]] usize effectCount() const;
    [[nodiscard]] IAudioEffect* effect(usize index) const;

    // Post-fader meters of the most recent processed block.
    [[nodiscard]] f32 peak() const;
    [[nodiscard]] f32 rms() const;
    // Current sidechain ducking gain applied to this bus (1 = not ducked).
    [[nodiscard]] f32 duckGain() const;
    // volume * mute * solo * parents' gains (ignores ducking).
    [[nodiscard]] f32 effectiveGain() const;

private:
    friend struct detail::EngineImpl;
    AudioBus(detail::EngineImpl& engine, std::string name, AudioBus* parent);

    detail::EngineImpl& m_engine;
    std::string m_name;
    AudioBus* m_parent = nullptr;
    std::vector<AudioBus*> m_children;
    f32 m_volume = 1.f;
    bool m_muted = false;
    bool m_solo = false;
    std::unique_ptr<detail::BusInternals> m_internals;
};

// Sidechain compression-like ducking: when `sidechainBus` RMS exceeds the threshold, `targetBus` is
// attenuated to `duckVolume`. Evaluated on the audio thread with attack/release smoothing.
struct DuckingSettings {
    std::string sidechainBus = "Voice";
    std::string targetBus = "Music";
    f32 threshold = 0.01f;  // linear RMS
    f32 duckVolume = 0.3f;  // linear gain while ducked
    f32 attackSeconds = 0.05f;
    f32 releaseSeconds = 0.5f;
};

// Named mixer state; applying one interpolates bus volumes over time (AudioEngine::update).
struct MixerSnapshot {
    std::string name;
    std::unordered_map<std::string, f32> busVolumes; // bus name → linear volume
    std::unordered_map<std::string, bool> busMuted;   // applied at the end of the transition
};

} // namespace ox::audio
