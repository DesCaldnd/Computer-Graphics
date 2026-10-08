// Guide chapter 12 «Аудио»: offline engine, sounds, handles, fades, voice limit.
#include <oxwald/audio/audio_engine.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace ox;
using namespace ox::audio;

namespace {

// RMS of an interleaved stereo buffer over [fromSec, toSec).
f64 rms(const std::vector<f32>& buf, u32 rate, f32 fromSec, f32 toSec) {
    const usize a = static_cast<usize>(fromSec * static_cast<f32>(rate)) * 2;
    const usize b = std::min(buf.size(), static_cast<usize>(toSec * static_cast<f32>(rate)) * 2);
    f64 sum = 0.0;
    for (usize i = a; i < b; ++i) {
        sum += static_cast<f64>(buf[i]) * buf[i];
    }
    return b > a ? std::sqrt(sum / static_cast<f64>(b - a)) : 0.0;
}

} // namespace

TEST(GuideAudioBasics, OfflineEngineRendersASine) {
    // Офлайн-режим: без аудиоустройства, звук «рендерится» вызовом render()/renderSeconds().
    AudioEngine audio;
    AudioEngineConfig cfg;
    cfg.offline = true;
    cfg.sampleRate = 48000;
    cfg.maxVoices = 32;
    ASSERT_TRUE(audio.init(cfg));
    ASSERT_NE(audio.bus("SFX"), nullptr); // шины по умолчанию: Master → Music/SFX/Voice/UI/Ambience

    const SoundId beep = audio.createSine(440.f, 0.5f); // генератор: бесконечный синус
    PlayParams p;
    p.bus = "UI";
    const SoundHandle h = audio.play(beep, p);
    ASSERT_TRUE(h.valid());

    // 0.5 с звука; между блоками по 512 кадров вызывается update(dt).
    const std::vector<f32> pcm = audio.renderSeconds(0.5f);
    ASSERT_EQ(pcm.size(), 24000u * 2); // 24000 кадров × 2 канала

    // Моно-синус с амплитудой 0.5 → RMS ≈ 0.5/√2 в каждом канале.
    EXPECT_NEAR(rms(pcm, 48000, 0.1f, 0.5f), 0.5 / std::sqrt(2.0), 0.03);
}

TEST(GuideAudioBasics, OneShotFromPcmFreesItsHandle) {
    AudioEngine audio;
    ASSERT_TRUE(audio.init({.offline = true}));

    // 0.1 с тона 1 кГц, сгенерированного в коде (в игре — audio.loadSound("sfx/shot.ogg")).
    std::vector<f32> tone(4800);
    for (usize i = 0; i < tone.size(); ++i) {
        tone[i] = 0.5f * std::sin(6.2831853f * 1000.f * static_cast<f32>(i) / 48000.f);
    }
    const SoundId shot = audio.createFromPcm(tone, /*channels*/ 1, /*sampleRate*/ 48000);
    EXPECT_NEAR(audio.soundDuration(shot), 0.1f, 1e-4f);

    const SoundHandle h = audio.play(shot);
    EXPECT_TRUE(audio.isPlaying(h));
    audio.renderSeconds(0.3f);
    audio.update(0.f);
    // Отыгравший one-shot освобождает голос — хэндл становится недействительным.
    EXPECT_FALSE(audio.isValid(h));

    // Зацикленный звук живёт, пока его не остановят.
    PlayParams loop;
    loop.loop = true;
    const SoundHandle music = audio.play(shot, loop);
    audio.renderSeconds(0.35f);
    EXPECT_TRUE(audio.isPlaying(music));
    audio.stop(music, /*fadeOutSeconds*/ 0.1f);
    audio.renderSeconds(0.2f);
    EXPECT_FALSE(audio.isValid(music));
}

TEST(GuideAudioBasics, FadesAndStartDelay) {
    AudioEngine audio;
    ASSERT_TRUE(audio.init({.offline = true}));
    const SoundId sine = audio.createSine(500.f, 0.5f);
    const f64 steady = 0.5 / std::sqrt(2.0);

    PlayParams p;
    p.startDelaySeconds = 0.2f; // начнёт звучать через 0.2 с
    p.fadeInSeconds = 0.1f;     // и плавно нарастёт за 0.1 с
    const SoundHandle h = audio.play(sine, p);
    auto pcm = audio.renderSeconds(0.6f, 256);
    EXPECT_LT(rms(pcm, 48000, 0.f, 0.19f), 1e-6);           // тишина до старта
    EXPECT_NEAR(rms(pcm, 48000, 0.4f, 0.6f), steady, 0.03); // после fade-in — полная громкость

    audio.fadeTo(h, 0.25f, 0.1f); // увести громкость до 0.25 за 0.1 с
    pcm = audio.renderSeconds(0.3f, 256);
    EXPECT_NEAR(rms(pcm, 48000, 0.15f, 0.3f) / steady, 0.25, 0.03);
}

TEST(GuideAudioBasics, VoiceLimitAndPriority) {
    AudioEngine audio;
    ASSERT_TRUE(audio.init({.offline = true, .maxVoices = 2}));
    const SoundId sine = audio.createSine(440.f, 0.2f);

    PlayParams quiet;
    quiet.volume = 0.2f;
    PlayParams loud;
    loud.volume = 1.f;
    const SoundHandle a = audio.play(sine, quiet);
    const SoundHandle b = audio.play(sine, loud);
    ASSERT_TRUE(a && b);

    // Все голоса заняты: новый звук «крадёт» самый тихий голос с тем же приоритетом…
    const SoundHandle c = audio.play(sine, loud);
    EXPECT_TRUE(c.valid());
    EXPECT_FALSE(audio.isValid(a));

    // …но более тихий звук того же приоритета не получает голос.
    PlayParams whisper;
    whisper.volume = 0.05f;
    EXPECT_FALSE(audio.play(sine, whisper).valid());

    // Высокий приоритет побеждает даже при малой громкости.
    whisper.priority = 10;
    EXPECT_TRUE(audio.play(sine, whisper).valid());
    EXPECT_EQ(audio.activeVoiceCount(), 2u);
}
