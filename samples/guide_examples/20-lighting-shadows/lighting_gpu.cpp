// Глава 20: освещение и тени на GPU — солнце с каскадами, прожектор и точечный свет с тенями, кэш теней,
// уровни качества, экспозиция (docs/guide/20-lighting-shadows.md). Без Vulkan-устройства тесты пропускаются.
#include "guide_render_scene.hpp"

#include <oxwald/core/scalability.hpp>

using namespace ox;
using namespace ox::render;
using guide::Image;
using guide::ScopedCVar;

namespace {

class GuideLightingGpu : public guide::GuideRenderTest {
protected:
    // Пол, куб и сфера; солнце из EnvironmentComponent.
    Entity sunScene() {
        const Uuid grey = material({0.7f, 0.7f, 0.7f, 1.0f}, 0.0f, 0.8f);
        mesh(Primitive::Plane, grey, {0, 0, 0}, glm::vec3(30.0f));
        mesh(Primitive::Cube, material({0.8f, 0.15f, 0.1f, 1.0f}, 0.0f, 0.5f), {0, 1, 0}, glm::vec3(2.0f));
        mesh(Primitive::Sphere, material({0.95f, 0.95f, 0.95f, 1.0f}, 1.0f, 0.2f), {2.5f, 0.75f, 1.0f}, glm::vec3(1.5f));

        Entity sun = world->create("Sun");
        sun.setRotation(lookRotation(glm::normalize(glm::vec3(-0.6f, -1.0f, -0.35f))));
        auto& l = sun.add<LightComponent>();
        l.type = LightType::Directional;
        l.intensity = 30000.0f; // лк
        l.castShadows = true;
        l.sourceRadius = 0.5f;  // градусы: полутень PCSS

        auto& env = world->create("Environment").add<EnvironmentComponent>();
        env.sun = EntityRef(sun.get<IdComponent>().id);
        return sun;
    }
};

} // namespace

TEST_F(GuideLightingGpu, SunShadowsOnlyDarken) {
    sunScene();
    const CameraParams cam = camera({2, 8, 12}, {0, 0, -1}, 13.5f);
    // Статистика проходов — из последнего завершённого GPU-кадра, поэтому рендерим несколько кадров.
    const Image lit = render(cam, 128, 128, 4);
    EXPECT_TRUE(ranPass("Shadow.Cascades"));
    EXPECT_TRUE(ranPass("ShadowMask"));
    Image unshadowed;
    {
        ScopedCVar off("r.Shadows", "false"); // граф пересобирается на следующем кадре, без перезапуска
        unshadowed = render(cam, 128, 128, 4);
        EXPECT_FALSE(ranPass("Shadow.Cascades"));
    }
    EXPECT_GT(guide::meanDifference(lit, unshadowed), 0.5);
    EXPECT_LT(lit.meanLuminance(), unshadowed.meanLuminance()); // тени только затемняют
}

TEST_F(GuideLightingGpu, LocalLightShadowsAreCached) {
    const Uuid grey = material({0.7f, 0.7f, 0.7f, 1.0f}, 0.0f, 0.5f);
    mesh(Primitive::Plane, grey, {0, 0, 0}, glm::vec3(20.0f));
    mesh(Primitive::Cube, grey, {0, 0.5f, 0});

    Entity spot = world->create("Spot");
    spot.setPosition({0, 5, 2});
    spot.setRotation(lookRotation(glm::normalize(glm::vec3(0, -5, -2))));
    auto& s = spot.add<LightComponent>();
    s.type = LightType::Spot;
    s.intensity = 20000.0f; // лм
    s.range = 12.0f;
    s.innerConeAngle = 20.0f;
    s.outerConeAngle = 35.0f;
    s.castShadows = true;

    Entity lamp = world->create("Lamp");
    lamp.setPosition({-6, 1.5f, 0});
    auto& p = lamp.add<LightComponent>();
    p.type = LightType::Point;
    p.intensity = 5000.0f;
    p.range = 4.0f;
    p.castShadows = true;

    const CameraParams cam = camera({0, 6, 10}, {0, 0, 0}, 6.0f);
    render(cam, 128, 128, 3);
    // Ничего не двигалось: тайл прожектора и 6 граней точечного источника взяты из кэша (r.Shadows.Caching).
    EXPECT_EQ(renderer->stats().shadowedLights, 2u);
    EXPECT_EQ(renderer->stats().shadowMapsRendered, 0u);
    EXPECT_EQ(renderer->stats().shadowMapsCached, 2u);

    ScopedCVar noCache("r.Shadows.Caching", "false");
    render(cam, 128, 128, 2);
    EXPECT_GT(renderer->stats().shadowMapsRendered, 0u); // без кэша — перерисовка каждый кадр
}

TEST_F(GuideLightingGpu, ShadowQualityLevelsRender) {
    sunScene();
    Entity lamp = world->create("Lamp");
    lamp.setPosition({-2, 1.5f, 2});
    auto& p = lamp.add<LightComponent>();
    p.type = LightType::Point;
    p.intensity = 3000.0f;
    p.range = 6.0f;
    p.castShadows = true;
    const CameraParams cam = camera({2, 8, 12}, {0, 0, -1}, 13.5f);
    for (QualityLevel l : {QualityLevel::Low, QualityLevel::Medium, QualityLevel::High, QualityLevel::Ultra}) {
        scalability::setGroup(Scalability::Shadows, l);
        const Image img = render(cam, 96, 96, 2);
        EXPECT_GT(img.meanLuminance(), 0.05f) << int(l);
        EXPECT_EQ(renderer->settings().pcss, l >= QualityLevel::High) << int(l);
    }
    scalability::setGroup(Scalability::Shadows, QualityLevel::High);
}

TEST_F(GuideLightingGpu, ManualExposure) {
    sunScene();
    const CameraParams cam = camera({2, 8, 12}, {0, 0, -1}, 13.5f);
    ScopedCVar mode("r.Exposure.Mode", "Manual"); // игнорировать EV100 камеры
    Image bright, dark;
    {
        ScopedCVar ev("r.Exposure.EV100", "12");
        bright = render(cam);
    }
    {
        ScopedCVar ev("r.Exposure.EV100", "15"); // +3 EV = в 8 раз меньше света на «сенсоре»
        dark = render(cam);
    }
    EXPECT_GT(bright.meanLuminance(), dark.meanLuminance() + 0.1f);
}
