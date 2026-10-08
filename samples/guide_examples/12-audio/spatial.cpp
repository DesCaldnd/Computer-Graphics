// Guide chapter 12 «Аудио»: 3D sound, listener, attenuation models.
#include <oxwald/audio/audio_engine.hpp>

#include <gtest/gtest.h>

#include <glm/gtc/quaternion.hpp>

#include <vector>

using namespace ox;
using namespace ox::audio;

namespace {

struct StereoEnergy {
    f64 left = 0.0, right = 0.0;
};

StereoEnergy energy(const std::vector<f32>& buf, usize fromFrame) {
    StereoEnergy e;
    for (usize i = fromFrame; i < buf.size() / 2; ++i) {
        e.left += static_cast<f64>(buf[i * 2]) * buf[i * 2];
        e.right += static_cast<f64>(buf[i * 2 + 1]) * buf[i * 2 + 1];
    }
    return e;
}

} // namespace

TEST(GuideAudioSpatial, SourceToTheRightIsLouderInTheRightChannel) {
    AudioEngine audio;
    ASSERT_TRUE(audio.init({.offline = true}));
    const SoundId engineHum = audio.createNoise(NoiseType::Pink, 0.3f, /*seed*/ 7);

    // Слушатель — камера в начале координат, смотрит в −Z (identity-ориентация).
    audio.setListener({.position = {0, 0, 0}, .orientation = glm::quat(1, 0, 0, 0)});

    PlayParams p;
    p.bus = "SFX";
    p.loop = true;
    p.spatial.enabled = true;          // 3D-звук
    p.spatial.position = {3.f, 0.f, 0.f}; // справа от слушателя
    p.spatial.attenuation = AttenuationModel::Inverse;
    p.spatial.minDistance = 1.f;
    p.spatial.maxDistance = 50.f;
    const SoundHandle h = audio.play(engineHum, p);
    ASSERT_TRUE(h.valid());

    StereoEnergy e = energy(audio.renderSeconds(0.4f), 4800);
    EXPECT_GT(e.right, 2.0 * e.left);

    // Источник переехал влево (скорость нужна для эффекта Доплера).
    audio.setPosition(h, {-3.f, 0.f, 0.f}, /*velocity*/ {-1.f, 0.f, 0.f});
    e = energy(audio.renderSeconds(0.4f), 4800);
    EXPECT_GT(e.left, 2.0 * e.right);

    // Развернули слушателя на 180° вокруг Y — стороны снова поменялись.
    audio.setListener({.orientation = glm::angleAxis(glm::radians(180.f), glm::vec3(0, 1, 0))});
    e = energy(audio.renderSeconds(0.4f), 4800);
    EXPECT_GT(e.right, 2.0 * e.left);
}

TEST(GuideAudioSpatial, AttenuationModels) {
    Spatial3D s;
    s.minDistance = 1.f;
    s.maxDistance = 21.f;

    s.attenuation = AttenuationModel::Inverse; // minD / (minD + rolloff·(d − minD))
    EXPECT_FLOAT_EQ(evaluateAttenuation(s, 0.5f), 1.f); // ближе minDistance — без ослабления
    EXPECT_NEAR(evaluateAttenuation(s, 4.f), 0.25f, 1e-5f);

    s.attenuation = AttenuationModel::Linear; // до нуля на maxDistance
    EXPECT_NEAR(evaluateAttenuation(s, 11.f), 0.5f, 1e-5f);
    EXPECT_NEAR(evaluateAttenuation(s, 30.f), 0.f, 1e-5f);

    s.attenuation = AttenuationModel::Exponential; // (d / minD)^−rolloff
    s.rolloff = 2.f;
    EXPECT_NEAR(evaluateAttenuation(s, 2.f), 0.25f, 1e-5f);

    // Своя кривая «дистанция → громкость», кусочно-линейная.
    s.attenuation = AttenuationModel::Custom;
    s.customCurve = {{0.f, 1.f}, {10.f, 0.5f}, {20.f, 0.f}};
    EXPECT_NEAR(evaluateAttenuation(s, 5.f), 0.75f, 1e-5f);
    EXPECT_NEAR(evaluateAttenuation(s, 25.f), 0.f, 1e-5f);

    // Децибелы ↔ линейное усиление.
    EXPECT_NEAR(dbToLinear(-6.f), 0.501f, 1e-3f);
    EXPECT_NEAR(linearToDb(0.5f), -6.02f, 1e-2f);
}

TEST(GuideAudioSpatial, DistanceLowPassAndCone) {
    AudioEngine audio;
    ASSERT_TRUE(audio.init({.offline = true}));
    const SoundId noise = audio.createNoise(NoiseType::White, 0.3f, 9);

    PlayParams p;
    p.spatial.enabled = true;
    p.spatial.position = {0.f, 0.f, -10.f};
    p.spatial.direction = {0.f, 0.f, 1.f}; // «динамик» смотрит на слушателя
    p.spatial.coneInnerAngle = glm::radians(60.f);
    p.spatial.coneOuterAngle = glm::radians(120.f);
    p.spatial.coneOuterGain = 0.2f;         // сзади динамика — 20 % громкости
    p.spatial.distanceLowPass = true;       // «поглощение воздухом»: далёкий звук глуше
    p.spatial.lowPassFarCutoff = 1500.f;
    const SoundHandle h = audio.play(noise, p);
    const StereoEnergy front = energy(audio.renderSeconds(0.3f), 4800);

    audio.setDirection(h, {0.f, 0.f, -1.f}); // отвернули динамик
    const StereoEnergy back = energy(audio.renderSeconds(0.3f), 4800);
    EXPECT_LT(back.left + back.right, 0.2 * (front.left + front.right));

    EXPECT_GT(audio.audibility(h), 0.f); // оценка громкости, по ней крадутся голоса
}
