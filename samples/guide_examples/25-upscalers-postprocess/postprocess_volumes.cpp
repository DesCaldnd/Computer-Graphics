// Глава 25: PostProcessVolume — CPU-часть (docs/guide/25-upscalers-postprocess.md). Глобальный объём + объём-коробка
// «пещера», вес по расстоянию до коробки, смешивание категорий по приоритету, extract на сцене, сериализация.
#include <oxwald/core/reflect.hpp>
#include <oxwald/core/serial/convert.hpp>
#include <oxwald/render/components/postprocess.hpp>
#include <oxwald/render/features/postprocess/postprocess.hpp>
#include <oxwald/render/render.hpp>
#include <oxwald/scene/component_registry.hpp>
#include <oxwald/scene/scene.hpp>
#include <oxwald/scene/world.hpp>

#include <gtest/gtest.h>

#include <vector>

using namespace ox;
using namespace ox::render;

namespace {

// Глобальные настройки уровня: лёгкий bloom и виньетка везде.
PostProcessVolumeSnapshot globalLook() {
    PostProcessVolumeSnapshot v;
    v.unbound = true;
    v.priority = 0;
    v.settings.overrideBloom = true;
    v.settings.bloomIntensity = 0.2f;
    v.settings.overrideLens = true;
    v.settings.vignetteIntensity = 0.5f;
    return v;
}

// Пещера: коробка 4×4×4 м в точке (10, 0, 0), обесцвечивание и сильный bloom, плавный вход за 2 м.
PostProcessVolumeSnapshot caveLook() {
    PostProcessVolumeSnapshot v;
    v.unbound = false;
    v.extents = glm::vec3(2.0f); // половинные размеры
    v.blendRadius = 2.0f;
    v.priority = 1;              // применяется после глобального → побеждает
    v.worldToLocal = glm::inverse(glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f)));
    v.settings.overrideBloom = true;
    v.settings.bloomIntensity = 0.6f;
    v.settings.overrideGrading = true;
    v.settings.saturation = 0.0f;
    return v;
}

} // namespace

TEST(GuidePostProcessVolumes, WeightsByDistance) {
    const PostProcessVolumeSnapshot cave = caveLook();
    EXPECT_FLOAT_EQ(postProcessVolumeWeight(cave, {10, 0, 0}), 1.0f);  // внутри коробки
    EXPECT_FLOAT_EQ(postProcessVolumeWeight(cave, {13, 0, 0}), 0.5f);  // 1 м снаружи при blendRadius 2 м
    EXPECT_FLOAT_EQ(postProcessVolumeWeight(cave, {20, 0, 0}), 0.0f);  // далеко
    EXPECT_FLOAT_EQ(postProcessVolumeWeight(globalLook(), {1000, 0, 0}), 1.0f); // unbound — везде blendWeight

    PostProcessVolumeSnapshot half = globalLook();
    half.blendWeight = 0.25f;
    EXPECT_FLOAT_EQ(postProcessVolumeWeight(half, {0, 0, 0}), 0.25f);
}

TEST(GuidePostProcessVolumes, BlendByPriorityPerCategory) {
    PostProcessSettings base; // обычно postProcessDefaults(settings) — значения cvar'ов r.Bloom.Intensity и т. п.
    base.bloomIntensity = 0.04f;
    const std::vector<PostProcessVolumeSnapshot> volumes{caveLook(), globalLook()}; // порядок в списке не важен

    const PostProcessSettings outside = blendPostProcessVolumes(base, volumes, {-50, 0, 0});
    EXPECT_FLOAT_EQ(outside.bloomIntensity, 0.2f);   // глобальный
    EXPECT_FLOAT_EQ(outside.vignetteIntensity, 0.5f);
    EXPECT_FLOAT_EQ(outside.saturation, 1.0f);       // категория Grading никем не переопределена

    const PostProcessSettings inside = blendPostProcessVolumes(base, volumes, {10, 0, 0});
    EXPECT_FLOAT_EQ(inside.bloomIntensity, 0.6f);    // пещера (приоритет 1) поверх глобального
    EXPECT_FLOAT_EQ(inside.saturation, 0.0f);
    EXPECT_FLOAT_EQ(inside.vignetteIntensity, 0.5f); // Lens пещера не трогает — остаётся глобальное

    const PostProcessSettings entrance = blendPostProcessVolumes(base, volumes, {13, 0, 0}); // вес пещеры 0.5
    EXPECT_NEAR(entrance.bloomIntensity, 0.4f, 1e-5f); // lerp(0.2, 0.6, 0.5)
    EXPECT_NEAR(entrance.saturation, 0.5f, 1e-5f);
}

TEST(GuidePostProcessVolumes, ComponentOnSceneAndSerialization) {
    registerSceneTypes();
    registerPostProcessTypes(); // вызывается и из render::registerRenderTypes()
    EXPECT_NE(ComponentRegistry::instance().find("PostProcessVolume"), nullptr);

    World world;
    Entity global = world.create("GlobalPostProcess");
    auto& g = global.add<PostProcessVolumeComponent>();       // unbound = true по умолчанию
    g.settings.overrideExposure = true;
    g.settings.autoExposure = true;
    g.settings.exposureCompensation = 0.5f;

    Entity cave = world.create("CavePostProcess");
    cave.setPosition({10.0f, 0.0f, 0.0f});
    auto& c = cave.add<PostProcessVolumeComponent>();
    c.unbound = false;
    c.extents = glm::vec3(2.0f);
    c.blendRadius = 2.0f;
    c.priority = 1;
    c.settings.overrideGrading = true;
    c.settings.saturation = 0.0f;
    c.settings.overrideWhiteBalance = true;
    c.settings.temperature = 4000.0f;  // нейтрализуем тёплый свет факелов → картинка холоднее

    world.updateTransforms();
    RenderSnapshot snap;
    extract(world, snap);
    RenderSettings rs;
    PostProcessSettings base;
    const PostProcessSettings atCave = resolvePostProcessSettings(snap, rs, {10, 0, 0});
    EXPECT_TRUE(atCave.autoExposure);
    EXPECT_FLOAT_EQ(atCave.exposureCompensation, 0.5f);
    EXPECT_FLOAT_EQ(atCave.saturation, 0.0f);
    EXPECT_FLOAT_EQ(atCave.temperature, 4000.0f);
    const PostProcessSettings far = resolvePostProcessSettings(snap, rs, {-100, 0, 0});
    EXPECT_FLOAT_EQ(far.saturation, base.saturation);

    // Компонент отражён: тип "PostProcessVolume", настройки сохраняются в сцену.
    EXPECT_EQ(reflect::typeOf<PostProcessVolumeComponent>().name, "PostProcessVolume");
    PostProcessVolumeComponent back;
    ASSERT_TRUE(serial::fromValue(serial::toValue(c), back));
    EXPECT_EQ(back.priority, 1);
    EXPECT_FLOAT_EQ(back.settings.temperature, 4000.0f);
}

TEST(GuidePostProcessVolumes, SettingsDefaults) {
    const PostProcessSettings s;
    EXPECT_FLOAT_EQ(s.minEV100, -4.0f);
    EXPECT_FLOAT_EQ(s.maxEV100, 20.0f);
    EXPECT_FLOAT_EQ(s.adaptationSpeedUp, 3.0f);
    EXPECT_FLOAT_EQ(s.adaptationSpeedDown, 1.0f);
    EXPECT_FLOAT_EQ(s.histogramLowPercent, 70.0f);
    EXPECT_FLOAT_EQ(s.histogramHighPercent, 95.0f);
    EXPECT_FLOAT_EQ(s.bloomIntensity, 0.04f);
    EXPECT_FLOAT_EQ(s.maxBokehSize, 1.5f);
    EXPECT_FLOAT_EQ(s.motionBlurAmount, 0.5f);
    EXPECT_FLOAT_EQ(s.motionBlurMax, 5.0f);
    EXPECT_FLOAT_EQ(s.temperature, 6500.0f);
    EXPECT_FLOAT_EQ(s.lutIntensity, 1.0f);
    const PostProcessVolumeComponent v;
    EXPECT_TRUE(v.enabled && v.unbound);
    EXPECT_EQ(v.extents, glm::vec3(5.0f));
    EXPECT_FLOAT_EQ(v.blendRadius, 1.0f);
    EXPECT_FLOAT_EQ(v.blendWeight, 1.0f);
    EXPECT_EQ(v.priority, 0);
}
