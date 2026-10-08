// Guide chapter 12 «Аудио»: buses, effects, ducking, snapshots, custom DSP.
#include <oxwald/audio/audio_engine.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <vector>

using namespace ox;
using namespace ox::audio;

namespace {

f64 rms(const std::vector<f32>& buf, usize fromSample = 9600) {
    f64 sum = 0.0;
    for (usize i = fromSample; i < buf.size(); ++i) {
        sum += static_cast<f64>(buf[i]) * buf[i];
    }
    return std::sqrt(sum / static_cast<f64>(buf.size() - fromSample));
}

// Energy of the first difference relative to the signal: a crude "brightness" measure.
f64 brightness(const std::vector<f32>& buf) {
    f64 diff = 0.0, sig = 0.0;
    for (usize i = 9602; i < buf.size(); i += 2) {
        const f64 d = buf[i] - buf[i - 2];
        diff += d * d;
        sig += static_cast<f64>(buf[i]) * buf[i];
    }
    return diff / sig;
}

// Свой DSP-эффект: простой «гейн». Работает на аудиопотоке, параметры — атомики.
class GainEffect final : public IAudioEffect {
public:
    explicit GainEffect(f32 gain) : m_gain(gain) {}
    void setGain(f32 g) { m_gain.store(g, std::memory_order_relaxed); }

    void prepare(u32 /*sampleRate*/, u32 channels) override { m_channels = channels; }
    void process(f32* frames, u32 frameCount) override {
        const f32 g = m_gain.load(std::memory_order_relaxed);
        for (u32 i = 0; i < frameCount * m_channels; ++i) {
            frames[i] *= g; // interleaved f32, обработка на месте
        }
    }
    [[nodiscard]] const char* typeName() const override { return "Gain"; }

private:
    std::atomic<f32> m_gain;
    u32 m_channels = 2;
};

} // namespace

TEST(GuideAudioMixer, BusHierarchyVolumeAndMute) {
    AudioEngine audio;
    ASSERT_TRUE(audio.init({.offline = true}));

    // Своя шина под SFX: Master → SFX → Footsteps.
    AudioBus* footsteps = audio.createBus("Footsteps", audio.bus("SFX"));
    ASSERT_NE(footsteps, nullptr);
    audio.bus("SFX")->setVolumeDb(-6.f); // ≈ 0.5
    footsteps->setVolume(0.5f);
    EXPECT_NEAR(footsteps->effectiveGain(), 0.25f, 0.01f); // громкости перемножаются по иерархии

    const SoundId step = audio.createSine(300.f, 0.5f);
    PlayParams p;
    p.bus = "Footsteps";
    audio.play(step, p);
    EXPECT_NEAR(rms(audio.renderSeconds(0.3f)), 0.25 * 0.5 / std::sqrt(2.0), 0.01);

    audio.bus("SFX")->setMuted(true); // глушит и все дочерние шины
    EXPECT_LT(rms(audio.renderSeconds(0.3f)), 1e-6);
    audio.bus("SFX")->setMuted(false);

    audio.bus("Music")->setSolo(true); // соло: всё, что не связано с Music, замолкает
    EXPECT_LT(rms(audio.renderSeconds(0.3f)), 1e-6);
    audio.bus("Music")->setSolo(false);
}

TEST(GuideAudioMixer, EffectChainOnABus) {
    AudioEngine audio;
    ASSERT_TRUE(audio.init({.offline = true}));
    const SoundId wind = audio.createNoise(NoiseType::White, 0.3f, 4);
    PlayParams p;
    p.bus = "Ambience";
    audio.play(wind, p);
    const f64 dry = brightness(audio.renderSeconds(0.3f));

    AudioBus* ambience = audio.bus("Ambience");
    auto* lp = ambience->addEffect<LowPassEffect>(800.f); // эффекты идут в порядке добавления
    auto* reverb = ambience->addEffect<ReverbEffect>(ReverbEffect::Params{.roomSize = 0.8f, .wet = 0.4f});
    auto* gain = ambience->addEffect<GainEffect>(0.5f);
    EXPECT_EQ(ambience->effectCount(), 3u);
    EXPECT_LT(brightness(audio.renderSeconds(0.3f)), dry * 0.2); // низкочастотный фильтр «приглушил» звук

    lp->setCutoff(2000.f);   // параметры можно менять «на лету» с игрового потока
    reverb->setBypass(true); // временно выключить эффект, не удаляя
    gain->setGain(1.f);
    EXPECT_TRUE(ambience->removeEffect(lp));
    ambience->clearEffects();
    EXPECT_EQ(ambience->effectCount(), 0u);
    EXPECT_NEAR(brightness(audio.renderSeconds(0.3f)) / dry, 1.0, 0.1);
}

TEST(GuideAudioMixer, VoiceDucksMusic) {
    AudioEngine audio;
    ASSERT_TRUE(audio.init({.offline = true}));
    // Когда в Voice кто-то говорит, музыка приглушается до 25 %.
    audio.addDucking({.sidechainBus = "Voice", .targetBus = "Music", .threshold = 0.01f, .duckVolume = 0.25f,
                      .attackSeconds = 0.02f, .releaseSeconds = 0.2f});

    PlayParams music;
    music.bus = "Music";
    audio.play(audio.createSine(220.f, 0.4f), music);
    audio.renderSeconds(0.3f);
    EXPECT_NEAR(audio.bus("Music")->duckGain(), 1.f, 1e-3f);

    PlayParams voice;
    voice.bus = "Voice";
    const SoundHandle line = audio.play(audio.createNoise(NoiseType::Pink, 0.3f, 11), voice);
    audio.renderSeconds(0.4f);
    EXPECT_LT(audio.bus("Music")->duckGain(), 0.3f);

    audio.stop(line);
    audio.renderSeconds(1.5f);
    EXPECT_GT(audio.bus("Music")->duckGain(), 0.95f); // реплика закончилась — музыка вернулась
}

TEST(GuideAudioMixer, SnapshotsBlendBusVolumes) {
    AudioEngine audio;
    ASSERT_TRUE(audio.init({.offline = true}));

    audio.defineSnapshot(audio.captureSnapshot("Gameplay")); // запомнить текущее состояние
    MixerSnapshot pause;
    pause.name = "Pause";
    pause.busVolumes = {{"Music", 0.2f}, {"SFX", 0.f}};
    pause.busMuted = {{"Ambience", true}}; // mute применяется в конце перехода
    audio.defineSnapshot(pause);

    ASSERT_TRUE(audio.applySnapshot("Pause", /*transitionSeconds*/ 1.f));
    audio.update(0.5f); // переход интерполируется в update()
    EXPECT_NEAR(audio.bus("Music")->volume(), 0.6f, 1e-4f);
    audio.update(0.6f);
    EXPECT_NEAR(audio.bus("Music")->volume(), 0.2f, 1e-4f);
    EXPECT_TRUE(audio.bus("Ambience")->muted());

    ASSERT_TRUE(audio.applySnapshot("Gameplay", 0.f)); // мгновенно
    EXPECT_NEAR(audio.bus("Music")->volume(), 1.f, 1e-4f);
    EXPECT_FALSE(audio.applySnapshot("NoSuchSnapshot"));
}
