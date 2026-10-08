// Глава 22: объёмные эффекты на GPU — высотный туман и лучи прожектора, локальный объём тумана, облака, уровни
// качества и переключатели (docs/guide/22-volumetrics.md). Без Vulkan-устройства тесты пропускаются.
#include "../20-lighting-shadows/guide_render_scene.hpp"

#include <oxwald/core/scalability.hpp>
#include <oxwald/render/features/volumetrics/volumetrics.hpp>

using namespace ox;
using namespace ox::render;
using guide::Image;
using guide::ScopedCVar;

namespace {

class GuideVolumetricsGpu : public guide::GuideRenderTest {
protected:
    void SetUp() override {
        GuideRenderTest::SetUp();
        scalability::setGroup(Scalability::Volumetrics, QualityLevel::High);
    }
    void TearDown() override {
        scalability::setGroup(Scalability::Volumetrics, QualityLevel::High);
        GuideRenderTest::TearDown();
    }
    EnvironmentComponent& fogEnvironment(f32 density, f32 falloff) {
        auto& env = world->create("Environment").add<EnvironmentComponent>();
        env.fogEnabled = true;
        env.fogDensity = density;
        env.fogHeightFalloff = falloff;
        env.fogColor = glm::vec3(1.0f);
        return env;
    }
};

} // namespace

TEST_F(GuideVolumetricsGpu, SpotLightConeInHeightFog) {
    mesh(Primitive::Plane, material({0.5f, 0.5f, 0.5f, 1.0f}, 0.0f, 0.8f), {0, 0, 0}, glm::vec3(60.0f));
    Entity spot = world->create("Spot");
    spot.setPosition({0.0f, 7.0f, -6.0f});
    spot.setRotation(lookRotation({0, -1, 0}, {1, 0, 0}));
    auto& l = spot.add<LightComponent>();
    l.type = LightType::Spot;
    l.intensity = 60000.0f;
    l.range = 14.0f;
    l.innerConeAngle = 14.0f;
    l.outerConeAngle = 26.0f;
    l.castShadows = true;

    EnvironmentComponent& env = fogEnvironment(0.06f, 0.0f); // однородный туман: falloff 0
    env.skyIntensity = 0.002f;                               // ночь: видно только конус
    env.ambientIntensity = 0.002f;

    const CameraParams cam = camera({0, 3.0f, 6.0f}, {0, 3.5f, -6.0f}, 4.0f, 60.0f, 100.0f);
    const Image img = render(cam, 128, 128, 8); // туман копит историю: несколько кадров
    EXPECT_TRUE(ranPass("Volumetrics.FogScatter"));
    // Конус в тумане ярче тумана рядом с ним.
    EXPECT_GT(img.meanLuminance(60, 30, 68, 50), img.meanLuminance(5, 30, 13, 50) + 0.03f);
}

TEST_F(GuideVolumetricsGpu, LocalFogVolume) {
    mesh(Primitive::Plane, material({0.06f, 0.06f, 0.06f, 1.0f}, 0.0f, 0.8f), {0, 0, 0}, glm::vec3(60.0f));
    Entity lamp = world->create("Lamp");
    lamp.setPosition({0.0f, 1.0f, -6.0f});
    auto& pl = lamp.add<LightComponent>();
    pl.type = LightType::Point;
    pl.intensity = 6000.0f;
    pl.range = 6.0f;
    pl.color = {1.0f, 0.5f, 0.2f};
    world->create("Environment").add<EnvironmentComponent>().skyIntensity = 0.01f;

    const CameraParams cam = camera({0, 1.6f, 5.0f}, {0, 1.3f, -6.0f}, 4.0f, 60.0f, 100.0f);
    const Image clear = render(cam, 128, 128, 2);

    Entity box = world->create("FogBox");
    box.setPosition({0.0f, 1.2f, -6.0f});
    auto& fv = box.add<FogVolumeComponent>();
    fv.shape = FogVolumeShape::Box;
    fv.extents = {3.5f, 1.2f, 1.5f};
    fv.density = 0.35f;
    fv.noiseIntensity = 0.6f;
    const Image foggy = render(cam, 128, 128, 8);
    EXPECT_TRUE(ranPass("Volumetrics.FogInject"));
    EXPECT_GT(foggy.meanLuminance(), clear.meanLuminance()); // свет лампы рассеивается в объёме
}

TEST_F(GuideVolumetricsGpu, CloudLayer) {
    mesh(Primitive::Plane, material({0.25f, 0.3f, 0.2f, 1.0f}, 0.0f, 0.9f), {0, 0, 0}, glm::vec3(400.0f));
    Entity sun = world->create("Sun");
    sun.setRotation(lookRotation(glm::normalize(glm::vec3(-0.3f, -1.0f, -0.4f))));
    auto& l = sun.add<LightComponent>();
    l.type = LightType::Directional;
    l.intensity = 80000.0f;
    world->create("Environment").add<EnvironmentComponent>();
    auto& cl = world->create("Clouds").add<CloudLayerComponent>();
    cl.coverage = 0.5f;
    cl.windSpeed = 0.0f;

    const CameraParams cam = camera({0, 2.0f, 0}, {0, 22.0f, -60.0f}, 14.0f, 75.0f, 1000.0f);
    const Image cloudy = render(cam, 128, 96, 16); // шахматка 1/16: полная картинка за 16 кадров
    EXPECT_TRUE(ranPass("Volumetrics.CloudTrace"));
    ScopedCVar off("r.VolumetricClouds", "false");
    const Image clearSky = render(cam, 128, 96, 4);
    EXPECT_FALSE(ranPass("Volumetrics.CloudTrace"));
    EXPECT_GT(guide::meanDifference(cloudy, clearSky), 1.0);
}

TEST_F(GuideVolumetricsGpu, QualityLevelsAndToggles) {
    mesh(Primitive::Plane, material({0.6f, 0.6f, 0.6f, 1.0f}, 0.0f, 0.8f), {0, 0, 0}, glm::vec3(100.0f));
    Entity sun = world->create("Sun");
    sun.setRotation(lookRotation(glm::normalize(glm::vec3(-0.45f, -0.45f, 1.0f))));
    sun.add<LightComponent>().type = LightType::Directional;
    fogEnvironment(0.03f, 0.05f);
    world->create("Clouds").add<CloudLayerComponent>();
    const CameraParams cam = camera({0, 2.5f, 8.0f}, {0, 6.0f, -12.0f}, 13.0f, 65.0f, 300.0f);

    for (QualityLevel lv : {QualityLevel::Low, QualityLevel::Medium, QualityLevel::High, QualityLevel::Ultra}) {
        scalability::setGroup(Scalability::Volumetrics, lv);
        render(cam, 96, 64, 4);
        EXPECT_TRUE(ranPass("Volumetrics.FogIntegrate")) << int(lv);
        EXPECT_EQ(ranPass("Volumetrics.CloudTrace"), lv != QualityLevel::Low) << int(lv); // Low: без облаков
    }
    ScopedCVar off("r.Feature.Volumetrics", "false"); // выключить всю фичу
    render(cam, 96, 64, 4);
    EXPECT_FALSE(ranPass("Volumetrics."));
}
