#include <oxwald/audio/audio_engine.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace ox;
using namespace ox::audio;

namespace {

constexpr u32 kRate = 48000;

struct Energy {
    f64 left = 0, right = 0;
    [[nodiscard]] f64 total() const { return left + right; }
};

Energy energy(const std::vector<f32>& buf, f32 fromSec, f32 toSec) {
    Energy e;
    const usize a = static_cast<usize>(fromSec * kRate), b = std::min(buf.size() / 2, static_cast<usize>(toSec * kRate));
    for (usize i = a; i < b; ++i) {
        e.left += static_cast<f64>(buf[i * 2]) * buf[i * 2];
        e.right += static_cast<f64>(buf[i * 2 + 1]) * buf[i * 2 + 1];
    }
    return e;
}

f64 rms(const std::vector<f32>& buf, f32 fromSec, f32 toSec) {
    const usize frames = static_cast<usize>((toSec - fromSec) * kRate);
    return std::sqrt(energy(buf, fromSec, toSec).total() / (2.0 * std::max<usize>(frames, 1)));
}

// Energy of the first difference (~ +6 dB/oct tilt) relative to the signal energy: a crude HF measure.
f64 highFrequencyRatio(const std::vector<f32>& buf, f32 fromSec, f32 toSec) {
    f64 diff = 0, sig = 0;
    const usize a = static_cast<usize>(fromSec * kRate) + 1, b = std::min(buf.size() / 2, static_cast<usize>(toSec * kRate));
    for (usize i = a; i < b; ++i) {
        const f64 d = buf[i * 2] - buf[(i - 1) * 2];
        diff += d * d;
        sig += static_cast<f64>(buf[i * 2]) * buf[i * 2];
    }
    return sig > 0 ? diff / sig : 0.0;
}

class AudioTest : public ::testing::Test {
protected:
    void SetUp() override {
        AudioEngineConfig cfg;
        cfg.offline = true;
        cfg.sampleRate = kRate;
        cfg.maxVoices = 16;
        ASSERT_TRUE(engine.init(cfg));
    }
    AudioEngine engine;
};

PlayParams spatialAt(glm::vec3 pos, std::string bus = "SFX") {
    PlayParams p;
    p.bus = std::move(bus);
    p.spatial.enabled = true;
    p.spatial.position = pos;
    return p;
}

void writeWav16(const std::filesystem::path& path, const std::vector<f32>& mono, u32 rate) {
    std::ofstream f(path, std::ios::binary);
    auto u32le = [&](u32 v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16le = [&](u16 v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const u32 dataBytes = static_cast<u32>(mono.size() * 2);
    f.write("RIFF", 4);
    u32le(36 + dataBytes);
    f.write("WAVEfmt ", 8);
    u32le(16);
    u16le(1);
    u16le(1);
    u32le(rate);
    u32le(rate * 2);
    u16le(2);
    u16le(16);
    f.write("data", 4);
    u32le(dataBytes);
    for (f32 s : mono) {
        const i16 v = static_cast<i16>(std::clamp(s, -1.f, 1.f) * 32767.f);
        f.write(reinterpret_cast<const char*>(&v), 2);
    }
}

} // namespace

TEST_F(AudioTest, OfflineEngineHasDefaultBuses) {
    EXPECT_TRUE(engine.offline());
    EXPECT_EQ(engine.sampleRate(), kRate);
    EXPECT_EQ(engine.channels(), 2u);
    ASSERT_NE(engine.master(), nullptr);
    for (const char* n : {"Music", "SFX", "Voice", "UI", "Ambience"}) {
        ASSERT_NE(engine.bus(n), nullptr) << n;
        EXPECT_EQ(engine.bus(n)->parent(), engine.master());
    }
    // Silence when nothing plays.
    const auto buf = engine.renderSeconds(0.1f);
    EXPECT_EQ(energy(buf, 0, 0.1f).total(), 0.0);
}

TEST_F(AudioTest, SineGeneratorProducesExpectedLevel) {
    const SoundId sine = engine.createSine(440.f, 0.5f);
    PlayParams p;
    p.bus = "UI";
    const SoundHandle h = engine.play(sine, p);
    ASSERT_TRUE(h.valid());
    const auto buf = engine.renderSeconds(0.5f);
    // Mono sine of amplitude 0.5 duplicated to both channels → RMS ≈ 0.5/√2.
    EXPECT_NEAR(rms(buf, 0.1f, 0.5f), 0.5 / std::sqrt(2.0), 0.03);
}

TEST_F(AudioTest, SpatialPanFollowsSourcePosition) {
    const SoundId noise = engine.createNoise(NoiseType::White, 0.3f, 7);
    SoundHandle h = engine.play(noise, spatialAt({3.f, 0.f, 0.f}));
    auto buf = engine.renderSeconds(0.4f);
    Energy right = energy(buf, 0.1f, 0.4f);
    EXPECT_GT(right.right, 2.0 * right.left) << "source at +X must be louder in the right channel";

    engine.setPosition(h, {-3.f, 0.f, 0.f});
    buf = engine.renderSeconds(0.4f);
    Energy left = energy(buf, 0.1f, 0.4f);
    EXPECT_GT(left.left, 2.0 * left.right);

    // Rotating the listener 180° around Y swaps sides again.
    ListenerState l;
    l.orientation = glm::angleAxis(glm::radians(180.f), glm::vec3(0, 1, 0));
    engine.setListener(l);
    buf = engine.renderSeconds(0.4f);
    Energy swapped = energy(buf, 0.1f, 0.4f);
    EXPECT_GT(swapped.right, 2.0 * swapped.left);

    // Straight ahead → balanced.
    engine.setListener({});
    engine.setPosition(h, {0.f, 0.f, -3.f});
    buf = engine.renderSeconds(0.4f);
    Energy centre = energy(buf, 0.1f, 0.4f);
    EXPECT_NEAR(centre.left / centre.right, 1.0, 0.1);
}

TEST(AudioAttenuation, ModelsMatchFormulas) {
    Spatial3D s;
    s.minDistance = 1.f;
    s.maxDistance = 21.f;
    s.attenuation = AttenuationModel::Inverse;
    EXPECT_FLOAT_EQ(evaluateAttenuation(s, 0.5f), 1.f);
    EXPECT_NEAR(evaluateAttenuation(s, 4.f), 0.25f, 1e-5f);
    s.attenuation = AttenuationModel::Linear;
    EXPECT_NEAR(evaluateAttenuation(s, 11.f), 0.5f, 1e-5f);
    EXPECT_NEAR(evaluateAttenuation(s, 50.f), 0.f, 1e-5f);
    s.attenuation = AttenuationModel::Exponential;
    s.rolloff = 2.f;
    EXPECT_NEAR(evaluateAttenuation(s, 2.f), 0.25f, 1e-5f);
    s.attenuation = AttenuationModel::Custom;
    s.customCurve = {{0.f, 1.f}, {10.f, 0.5f}, {20.f, 0.f}};
    EXPECT_NEAR(evaluateAttenuation(s, 5.f), 0.75f, 1e-5f);
    EXPECT_NEAR(evaluateAttenuation(s, 15.f), 0.25f, 1e-5f);
    EXPECT_NEAR(evaluateAttenuation(s, 30.f), 0.f, 1e-5f);
}

TEST_F(AudioTest, RenderedAttenuationFollowsModel) {
    const SoundId noise = engine.createNoise(NoiseType::White, 0.3f, 3);
    for (AttenuationModel model : {AttenuationModel::Inverse, AttenuationModel::Linear, AttenuationModel::Exponential,
                                   AttenuationModel::Custom}) {
        PlayParams p = spatialAt({0, 0, -2.f});
        p.spatial.attenuation = model;
        p.spatial.minDistance = 1.f;
        p.spatial.maxDistance = 20.f;
        p.spatial.customCurve = {{0.f, 1.f}, {20.f, 0.f}};
        const SoundHandle h = engine.play(noise, p);
        auto near = engine.renderSeconds(0.3f);
        engine.setPosition(h, {0, 0, -8.f});
        auto far = engine.renderSeconds(0.3f);
        engine.stop(h);
        const f64 measured = std::sqrt(energy(far, 0.1f, 0.3f).total() / energy(near, 0.1f, 0.3f).total());
        Spatial3D s = p.spatial;
        const f64 expected = evaluateAttenuation(s, 8.f) / evaluateAttenuation(s, 2.f);
        EXPECT_NEAR(measured, expected, 0.05 + 0.08 * expected) << "model " << static_cast<int>(model);
    }
}

TEST_F(AudioTest, BusVolumeMuteAndSolo) {
    const SoundId sine = engine.createSine(300.f, 0.5f);
    PlayParams p;
    p.bus = "SFX";
    engine.play(sine, p);
    const f64 full = rms(engine.renderSeconds(0.3f), 0.1f, 0.3f);
    ASSERT_GT(full, 0.1);

    engine.bus("SFX")->setVolume(0.5f);
    EXPECT_NEAR(rms(engine.renderSeconds(0.3f), 0.1f, 0.3f) / full, 0.5, 0.02);
    EXPECT_NEAR(engine.bus("SFX")->effectiveGain(), 0.5f, 1e-6f);

    engine.master()->setVolume(0.5f);
    EXPECT_NEAR(rms(engine.renderSeconds(0.3f), 0.1f, 0.3f) / full, 0.25, 0.02);
    engine.master()->setVolume(1.f);
    engine.bus("SFX")->setVolume(1.f);

    engine.bus("SFX")->setMuted(true);
    EXPECT_LT(rms(engine.renderSeconds(0.3f), 0.1f, 0.3f), 1e-6);
    engine.bus("SFX")->setMuted(false);

    // Soloing another bus silences SFX; soloing SFX itself keeps it.
    engine.bus("Music")->setSolo(true);
    EXPECT_LT(rms(engine.renderSeconds(0.3f), 0.1f, 0.3f), 1e-6);
    engine.bus("SFX")->setSolo(true);
    EXPECT_NEAR(rms(engine.renderSeconds(0.3f), 0.1f, 0.3f) / full, 1.0, 0.02);
}

TEST_F(AudioTest, HierarchicalCustomBus) {
    AudioBus* footsteps = engine.createBus("Footsteps", engine.bus("SFX"));
    ASSERT_NE(footsteps, nullptr);
    EXPECT_EQ(footsteps->parent(), engine.bus("SFX"));
    engine.bus("SFX")->setVolume(0.5f);
    footsteps->setVolume(0.5f);
    const SoundId sine = engine.createSine(300.f, 0.5f);
    PlayParams p;
    p.bus = "Footsteps";
    engine.play(sine, p);
    const f64 r = rms(engine.renderSeconds(0.3f), 0.1f, 0.3f);
    EXPECT_NEAR(r, 0.25 * 0.5 / std::sqrt(2.0), 0.01);
}

TEST_F(AudioTest, VoiceDucksMusic) {
    engine.addDucking({.sidechainBus = "Voice", .targetBus = "Music", .threshold = 0.01f, .duckVolume = 0.25f,
                       .attackSeconds = 0.02f, .releaseSeconds = 0.2f});
    const SoundId music = engine.createSine(220.f, 0.4f);
    const SoundId voice = engine.createNoise(NoiseType::Pink, 0.3f, 11);
    PlayParams pm;
    pm.bus = "Music";
    engine.play(music, pm);
    engine.renderSeconds(0.3f);
    const f32 musicAlone = engine.bus("Music")->rms();
    EXPECT_NEAR(engine.bus("Music")->duckGain(), 1.f, 1e-3f);

    PlayParams pv;
    pv.bus = "Voice";
    const SoundHandle v = engine.play(voice, pv);
    engine.renderSeconds(0.4f);
    const f32 musicDucked = engine.bus("Music")->rms();
    EXPECT_LT(engine.bus("Music")->duckGain(), 0.3f);
    EXPECT_LT(musicDucked, musicAlone * 0.35f);

    engine.stop(v);
    engine.renderSeconds(1.5f);
    EXPECT_GT(engine.bus("Music")->duckGain(), 0.95f) << "music recovers after release";
}

TEST(AudioVoices, StealsQuietestAndRespectsPriority) {
    AudioEngine engine;
    AudioEngineConfig cfg;
    cfg.offline = true;
    cfg.maxVoices = 4;
    ASSERT_TRUE(engine.init(cfg));
    const SoundId sine = engine.createSine(440.f, 0.2f);
    std::vector<SoundHandle> hs;
    for (f32 vol : {1.f, 0.2f, 0.8f, 0.5f}) {
        PlayParams p;
        p.volume = vol;
        hs.push_back(engine.play(sine, p));
        ASSERT_TRUE(hs.back().valid());
    }
    EXPECT_EQ(engine.activeVoiceCount(), 4u);

    PlayParams loud;
    loud.volume = 0.9f;
    const SoundHandle h5 = engine.play(sine, loud);
    ASSERT_TRUE(h5.valid());
    EXPECT_FALSE(engine.isValid(hs[1])) << "quietest voice (0.2) is stolen";
    EXPECT_TRUE(engine.isValid(hs[0]));
    EXPECT_EQ(engine.activeVoiceCount(), 4u);

    // A quieter sound of the same priority cannot steal.
    PlayParams quiet;
    quiet.volume = 0.1f;
    EXPECT_FALSE(engine.play(sine, quiet).valid());

    // Higher priority wins even when quiet; low priority voices are victims first.
    PlayParams important;
    important.volume = 0.05f;
    important.priority = 10;
    const SoundHandle hi = engine.play(sine, important);
    ASSERT_TRUE(hi.valid());
    EXPECT_FALSE(engine.isValid(hs[3])) << "next quietest (0.5) stolen";

    // Fill with priority 10, then a priority 0 sound is rejected.
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(engine.play(sine, important).valid());
    }
    PlayParams low;
    low.volume = 1.f;
    EXPECT_FALSE(engine.play(sine, low).valid());
}

TEST_F(AudioTest, OneShotEndsAndLoopKeepsPlaying) {
    // 0.1 s of a 1 kHz tone.
    std::vector<f32> tone(kRate / 10);
    for (usize i = 0; i < tone.size(); ++i) {
        tone[i] = 0.5f * std::sin(6.2831853f * 1000.f * static_cast<f32>(i) / kRate);
    }
    const SoundId id = engine.createFromPcm(tone, 1, kRate);
    EXPECT_NEAR(engine.soundDuration(id), 0.1f, 1e-4f);

    PlayParams p;
    const SoundHandle once = engine.play(id, p);
    auto buf = engine.renderSeconds(0.3f, 256);
    EXPECT_GT(rms(buf, 0.0f, 0.095f), 0.2);
    EXPECT_LT(rms(buf, 0.11f, 0.3f), 1e-6);
    engine.update(0.f);
    EXPECT_FALSE(engine.isValid(once)) << "finished one-shot releases its voice";

    p.loop = true;
    const SoundHandle looped = engine.play(id, p);
    buf = engine.renderSeconds(0.35f, 256);
    EXPECT_GT(rms(buf, 0.25f, 0.35f), 0.2);
    EXPECT_TRUE(engine.isPlaying(looped));
    engine.setLooping(looped, false);
    engine.renderSeconds(0.2f);
    EXPECT_FALSE(engine.isValid(looped));
}

TEST_F(AudioTest, PauseResumeAndStartDelay) {
    const SoundId sine = engine.createSine(500.f, 0.5f);
    PlayParams p;
    p.startDelaySeconds = 0.2f;
    const SoundHandle h = engine.play(sine, p);
    auto buf = engine.renderSeconds(0.4f, 256);
    EXPECT_LT(rms(buf, 0.f, 0.19f), 1e-6);
    EXPECT_GT(rms(buf, 0.22f, 0.4f), 0.2);

    engine.pause(h);
    EXPECT_FALSE(engine.isPlaying(h));
    EXPECT_TRUE(engine.isValid(h));
    buf = engine.renderSeconds(0.2f);
    EXPECT_LT(rms(buf, 0.f, 0.2f), 1e-6);
    engine.resume(h);
    buf = engine.renderSeconds(0.2f);
    EXPECT_GT(rms(buf, 0.f, 0.2f), 0.2);
}

TEST_F(AudioTest, FadeInAndFadeOutTiming) {
    const SoundId sine = engine.createSine(1000.f, 0.5f);
    PlayParams p;
    p.fadeInSeconds = 0.5f;
    const SoundHandle h = engine.play(sine, p);
    auto buf = engine.renderSeconds(0.8f, 256);
    const f64 steady = 0.5 / std::sqrt(2.0);
    EXPECT_LT(rms(buf, 0.f, 0.05f), steady * 0.1);
    EXPECT_NEAR(rms(buf, 0.23f, 0.27f) / steady, 0.5, 0.06);
    EXPECT_NEAR(rms(buf, 0.6f, 0.8f) / steady, 1.0, 0.03);

    engine.stop(h, 0.2f);
    buf = engine.renderSeconds(0.4f, 256);
    EXPECT_NEAR(rms(buf, 0.08f, 0.12f) / steady, 0.5, 0.08);
    EXPECT_LT(rms(buf, 0.21f, 0.4f), 1e-6);
    EXPECT_FALSE(engine.isValid(h));

    // fadeTo on a running sound.
    const SoundHandle h2 = engine.play(sine, {});
    engine.renderSeconds(0.1f);
    engine.fadeTo(h2, 0.25f, 0.1f);
    buf = engine.renderSeconds(0.3f, 256);
    EXPECT_NEAR(rms(buf, 0.15f, 0.3f) / steady, 0.25, 0.03);
}

namespace {
struct MockOcclusion final : IAudioOcclusionProvider {
    f32 value = 0.f;
    int calls = 0;
    f32 occlusion(const glm::vec3&, const glm::vec3&) override {
        ++calls;
        return value;
    }
};
} // namespace

TEST_F(AudioTest, OcclusionLowPassesAndAttenuates) {
    MockOcclusion mock;
    engine.setOcclusionProvider(&mock);
    const SoundId noise = engine.createNoise(NoiseType::White, 0.3f, 5);
    PlayParams p = spatialAt({0, 0, -1.f});
    p.spatial.occlusion = true;
    const SoundHandle h = engine.play(noise, p);
    auto clear = engine.renderSeconds(0.5f);
    const f64 hfClear = highFrequencyRatio(clear, 0.2f, 0.5f);
    const f64 eClear = energy(clear, 0.2f, 0.5f).total();
    EXPECT_NEAR(engine.occlusion(h), 0.f, 1e-6f);

    mock.value = 1.f;
    engine.renderSeconds(1.f); // let the smoothing converge
    EXPECT_GT(engine.occlusion(h), 0.95f);
    auto occluded = engine.renderSeconds(0.5f);
    const f64 hfOcc = highFrequencyRatio(occluded, 0.f, 0.5f);
    const f64 eOcc = energy(occluded, 0.f, 0.5f).total();
    EXPECT_GT(mock.calls, 0);
    EXPECT_LT(hfOcc, hfClear * 0.2) << "occlusion low-pass removes high frequencies";
    EXPECT_LT(eOcc, eClear);

    // Gain part: a 100 Hz tone passes the occlusion low-pass, so its level drops by occlusionVolume (0.35).
    engine.stop(h);
    const SoundId low = engine.createSine(100.f, 0.4f);
    mock.value = 0.f;
    const SoundHandle hl = engine.play(low, p);
    const f64 lowClear = rms(engine.renderSeconds(0.5f), 0.2f, 0.5f);
    mock.value = 1.f;
    engine.renderSeconds(1.f);
    const f64 lowOcc = rms(engine.renderSeconds(0.5f), 0.f, 0.5f);
    EXPECT_NEAR(lowOcc / lowClear, 0.35, 0.05);
    engine.stop(hl);

    // Raycast adapter: 2 hits at 0.5 → 0.75.
    RaycastOcclusionProvider rays([](const glm::vec3&, const glm::vec3&) { return 2u; }, 0.5f);
    EXPECT_NEAR(rays.occlusion({}, {1, 0, 0}), 0.75f, 1e-6f);
}

TEST_F(AudioTest, DistanceLowPassDarkensFarSources) {
    const SoundId noise = engine.createNoise(NoiseType::White, 0.3f, 9);
    PlayParams p = spatialAt({0, 0, -1.f});
    p.spatial.attenuation = AttenuationModel::None;
    p.spatial.distanceLowPass = true;
    p.spatial.maxDistance = 50.f;
    p.spatial.lowPassFarCutoff = 1000.f;
    const SoundHandle h = engine.play(noise, p);
    const f64 nearHf = highFrequencyRatio(engine.renderSeconds(0.3f), 0.1f, 0.3f);
    engine.setPosition(h, {0, 0, -50.f});
    const f64 farHf = highFrequencyRatio(engine.renderSeconds(0.3f), 0.1f, 0.3f);
    EXPECT_LT(farHf, nearHf * 0.2);
}

TEST_F(AudioTest, SnapshotInterpolatesBusVolumes) {
    MixerSnapshot pause;
    pause.name = "Pause";
    pause.busVolumes = {{"Music", 0.2f}, {"SFX", 0.f}};
    pause.busMuted = {{"Ambience", true}};
    engine.defineSnapshot(pause);
    const MixerSnapshot normal = engine.captureSnapshot("Normal");
    engine.defineSnapshot(normal);

    ASSERT_TRUE(engine.applySnapshot("Pause", 1.f));
    engine.update(0.5f);
    EXPECT_NEAR(engine.bus("Music")->volume(), 0.6f, 1e-4f);
    EXPECT_NEAR(engine.bus("SFX")->volume(), 0.5f, 1e-4f);
    EXPECT_FALSE(engine.bus("Ambience")->muted());
    engine.update(0.6f);
    EXPECT_NEAR(engine.bus("Music")->volume(), 0.2f, 1e-4f);
    EXPECT_TRUE(engine.bus("Ambience")->muted());

    ASSERT_TRUE(engine.applySnapshot("Normal", 0.f));
    EXPECT_NEAR(engine.bus("Music")->volume(), 1.f, 1e-4f);
    EXPECT_FALSE(engine.bus("Ambience")->muted());
    EXPECT_FALSE(engine.applySnapshot("Missing"));
}

TEST_F(AudioTest, BusEffectsChain) {
    const SoundId noise = engine.createNoise(NoiseType::White, 0.3f, 4);
    PlayParams p;
    p.bus = "Ambience";
    engine.play(noise, p);
    const f64 dry = highFrequencyRatio(engine.renderSeconds(0.3f), 0.1f, 0.3f);
    auto* lp = engine.bus("Ambience")->addEffect<LowPassEffect>(800.f);
    ASSERT_NE(lp, nullptr);
    EXPECT_EQ(engine.bus("Ambience")->effectCount(), 1u);
    const f64 wet = highFrequencyRatio(engine.renderSeconds(0.3f), 0.1f, 0.3f);
    EXPECT_LT(wet, dry * 0.1);
    auto* rev = engine.bus("Ambience")->addEffect<ReverbEffect>();
    EXPECT_EQ(engine.bus("Ambience")->effectCount(), 2u);
    EXPECT_GT(rms(engine.renderSeconds(0.3f), 0.1f, 0.3f), 0.0);
    EXPECT_TRUE(engine.bus("Ambience")->removeEffect(lp));
    EXPECT_TRUE(engine.bus("Ambience")->removeEffect(rev));
    const f64 back = highFrequencyRatio(engine.renderSeconds(0.3f), 0.1f, 0.3f);
    EXPECT_NEAR(back / dry, 1.0, 0.1);
}

TEST(AudioEffects, StandaloneDsp) {
    // Delay: an impulse comes back after the delay time.
    DelayEffect delay(0.01f, 0.5f, 1.f, 1.f);
    delay.prepare(kRate, 1);
    std::vector<f32> buf(2000, 0.f);
    buf[0] = 1.f;
    delay.process(buf.data(), static_cast<u32>(buf.size()));
    EXPECT_FLOAT_EQ(buf[480], 1.f);
    EXPECT_FLOAT_EQ(buf[960], 0.5f);

    // Reverb: an impulse produces a decaying tail.
    ReverbEffect reverb;
    reverb.prepare(kRate, 2);
    std::vector<f32> st(kRate * 2, 0.f);
    st[0] = st[1] = 1.f;
    reverb.process(st.data(), kRate);
    f64 tail = 0;
    for (usize i = kRate / 10 * 2; i < st.size(); ++i) {
        tail += std::abs(st[i]);
    }
    EXPECT_GT(tail, 0.1);

    // High-pass removes DC.
    HighPassEffect hp(200.f);
    hp.prepare(kRate, 1);
    std::vector<f32> dc(kRate / 2, 1.f);
    hp.process(dc.data(), static_cast<u32>(dc.size()));
    EXPECT_LT(std::abs(dc.back()), 1e-3f);
}

TEST_F(AudioTest, LoadWavDecodedAndStreamed) {
    const auto path = std::filesystem::temp_directory_path() / "ox_audio_test_tone.wav";
    std::vector<f32> tone(kRate / 4);
    for (usize i = 0; i < tone.size(); ++i) {
        tone[i] = 0.5f * std::sin(6.2831853f * 440.f * static_cast<f32>(i) / kRate);
    }
    writeWav16(path, tone, kRate);

    for (LoadMode mode : {LoadMode::Decode, LoadMode::Stream}) {
        const SoundId id = engine.loadSound(path.string(), mode);
        ASSERT_TRUE(id.valid());
        EXPECT_NEAR(engine.soundDuration(id), 0.25f, 1e-3f);
        const SoundHandle h = engine.play(id, {});
        ASSERT_TRUE(h.valid());
        auto buf = engine.renderSeconds(0.4f, 256);
        EXPECT_NEAR(rms(buf, 0.05f, 0.2f), 0.5 / std::sqrt(2.0), 0.03);
        EXPECT_LT(rms(buf, 0.27f, 0.4f), 1e-4);
        engine.unloadSound(id);
    }
    EXPECT_FALSE(engine.loadSound("/nonexistent/file.ogg").valid());
    std::filesystem::remove(path);
}

TEST_F(AudioTest, DebugDrawEmitsLines) {
    const SoundId sine = engine.createSine(440.f);
    engine.play(sine, spatialAt({1, 0, 0}));
    int lines = 0;
    engine.debugDraw([&](glm::vec3, glm::vec3, glm::vec4) { ++lines; });
    EXPECT_GT(lines, 60);
}
