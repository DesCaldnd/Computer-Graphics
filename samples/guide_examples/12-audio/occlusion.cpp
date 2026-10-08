// Guide chapter 12 «Аудио»: occlusion through physics raycasts.
#include <oxwald/audio/audio_engine.hpp>
#include <oxwald/physics/physics.hpp>

#include <gtest/gtest.h>

#include <glm/geometric.hpp>

using namespace ox;

namespace {

// Провайдер окклюзии поверх физики: считаем, сколько тел пересекает отрезок слушатель → источник.
audio::RaycastOcclusionProvider makePhysicsOcclusion(const physics::PhysicsWorld& world) {
    return audio::RaycastOcclusionProvider(
        [&world](const glm::vec3& from, const glm::vec3& to) -> u32 {
            const glm::vec3 d = to - from;
            const f32 len = glm::length(d);
            if (len < 1e-4f) {
                return 0u;
            }
            return static_cast<u32>(world.raycastAll(from, d, len).size());
        },
        /*perHitOcclusion*/ 0.6f); // каждая стена «съедает» 60 %: occlusion = 1 − 0.4^hits
}

} // namespace

TEST(GuideAudioOcclusion, WallBetweenListenerAndSourceMuffles) {
    // Физический мир со стеной толщиной 0.5 м на z = −5.
    physics::PhysicsWorldDesc wd;
    wd.workerThreads = 0;
    physics::PhysicsWorld world(wd);
    physics::BodyDesc wall;
    wall.shape = physics::createShape(physics::ShapeDesc::box({4.f, 3.f, 0.25f}));
    wall.position = {0.f, 0.f, -5.f};
    wall.motionType = physics::MotionType::Static;
    world.createBody(wall);
    world.step(1.f / 60.f);

    audio::RaycastOcclusionProvider occlusion = makePhysicsOcclusion(world);

    audio::AudioEngine audio;
    ASSERT_TRUE(audio.init({.offline = true}));
    audio.setOcclusionProvider(&occlusion); // не владеет: провайдер должен пережить движок или быть снят

    audio::PlayParams p;
    p.loop = true;
    p.spatial.enabled = true;
    p.spatial.position = {0.f, 0.f, -10.f}; // за стеной
    p.spatial.occlusion = true;             // спрашивать провайдера для этого звука
    const audio::SoundHandle radio = audio.play(audio.createNoise(audio::NoiseType::Pink, 0.3f, 3), p);

    audio.renderSeconds(1.f); // окклюзия сглаживается во времени (occlusionSmoothing)
    EXPECT_NEAR(audio.occlusion(radio), 0.6f, 0.05f);

    // Источник вышел из-за стены — окклюзия уходит.
    audio.setPosition(radio, {16.f, 0.f, -10.f});
    audio.renderSeconds(1.f);
    EXPECT_LT(audio.occlusion(radio), 0.05f);

    audio.setOcclusionProvider(nullptr);
}
