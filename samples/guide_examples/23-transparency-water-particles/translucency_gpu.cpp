// Глава 23: стекло, вода и частицы на настоящем рендерере (headless). Нужен Vulkan-устройство; без него — пропуск.
// Проверяет, что нужные проходы графа запускаются и переключаются cvar'ами; пиксели точно не сравниваются.
#include "guide_render_scene.hpp"

#include <oxwald/assets/material.hpp>
#include <oxwald/core/scalability.hpp>
#include <oxwald/render/components/translucency.hpp>
#include <oxwald/render/features/translucency/translucency.hpp>

#include <filesystem>

using namespace ox;
using namespace ox::render;
using guide::CVarScope;

namespace {

class GuideTranslucencyGpu : public guide::RenderScene {
protected:
    void SetUp() override {
        RenderScene::SetUp();
        if (!device) return;
        registerTranslucencyTypes();
        for (Scalability g : {Scalability::Effects, Scalability::Shading, Scalability::Reflections}) {
            scalability::setGroup(g, QualityLevel::High);
        }
    }
};

} // namespace

TEST_F(GuideTranslucencyGpu, GlassPanesAndRefractiveSphere) {
    mesh(Primitive::Plane, solid({0.6f, 0.6f, 0.6f, 1.0f}), {0, 0, 0}, glm::vec3(12.0f));
    mesh(Primitive::Cube, solid({0.8f, 0.3f, 0.2f, 1.0f}), {0, 0.75f, -2.2f}, {3.0f, 1.5f, 0.3f});

    // Прозрачное стекло (Transparent) без смещения очереди — идёт через OIT (или сортировку по r.Translucency.Method).
    assets::MaterialAsset pane;
    pane.blendMode = assets::BlendMode::Transparent;
    pane.baseColor = {0.1f, 0.9f, 0.2f, 0.45f};
    pane.roughness = 0.05f;
    mesh(Primitive::Cube, addMaterial(pane), {0.0f, 0.8f, 0.0f}, {1.1f, 1.2f, 0.03f});
    // Окно из .oxmat главы: renderQueueOffset = 1 → всегда сортированный путь.
    const std::filesystem::path dir = OX_GUIDE_DIR;
    const Uuid window = addMaterial(*assets::loadMaterialFile(dir / "tinted_window.oxmat"));
    mesh(Primitive::Cube, window, {-0.5f, 0.9f, 0.6f}, {1.1f, 1.2f, 0.03f});
    // Преломляющее стекло (Refractive) с поглощением по Бугеру — Ламберту.
    const Uuid glass = addMaterial(*assets::loadMaterialFile(dir / "glass.oxmat"));
    mesh(Primitive::Sphere, glass, {0.8f, 0.7f, 0.3f}, glm::vec3(1.2f));
    sunAndSky();

    const CameraParams cam = CameraParams::lookAt({0.0f, 1.6f, 4.5f}, {0.0f, 0.7f, 0.0f}, 50.0f);
    {
        CVarScope method("r.Translucency.Method", "OIT");
        renderFrames(cam, 4);
        EXPECT_TRUE(ranPass("Translucency.Refractive"));
        EXPECT_TRUE(ranPass("Translucency.OIT"));    // зелёное стекло
        EXPECT_TRUE(ranPass("Translucency.Sorted")); // окно с renderQueueOffset != 0
    }
    {
        CVarScope method("r.Translucency.Method", "Sorted");
        renderFrames(cam, 4);
        EXPECT_TRUE(ranPass("Translucency.Sorted"));
        EXPECT_FALSE(ranPass("Translucency.OIT"));
    }
}

TEST_F(GuideTranslucencyGpu, WaterAndParticles) {
    mesh(Primitive::Cube, solid({0.76f, 0.68f, 0.5f, 1.0f}, 0.9f), {0.0f, -0.5f, 0.0f}, {30.0f, 1.0f, 30.0f});
    Entity lake = world->create("Lake");
    lake.setPosition({0.0f, 0.25f, 0.0f});
    lake.add<WaterSurfaceComponent>().size = {40.0f, 40.0f}; // пустой waves → зыбь по умолчанию

    Entity fire = world->create("Sparks");
    fire.setPosition({0.0f, 1.0f, 0.0f});
    auto& k = fire.add<ParticleEmitterComponent>();
    k.spawnRate = 200.0f;
    k.shape = ParticleShape::Cone;
    k.blend = ParticleBlend::Additive;
    k.emissive = 4.0f;
    sunAndSky();

    const CameraParams cam = CameraParams::lookAt({0.0f, 3.0f, 6.5f}, {0.0f, 0.5f, 0.0f}, 55.0f);
    const std::vector<u8> on = renderFrames(cam, 6);
    EXPECT_TRUE(ranPass("Water.Surface"));
    EXPECT_TRUE(ranPass("Particles.Simulate"));
    EXPECT_TRUE(ranPass("Particles.Composite"));
    EXPECT_FALSE(ranPass("Water.Underwater")) << "камера над водой";

    // Те же эффекты можно выключить cvar'ами — проходы исчезают из графа.
    CVarScope water("r.Water", "false");
    CVarScope particles("r.Particles", "false");
    const std::vector<u8> off = renderFrames(cam, 4);
    EXPECT_FALSE(ranPass("Water.Surface"));
    EXPECT_FALSE(ranPass("Particles.Simulate"));
    EXPECT_GT(meanAbsDifference(on, off), 0.5) << "вода и частицы меняют картинку";
}

TEST_F(GuideTranslucencyGpu, UnderwaterCamera) {
    mesh(Primitive::Cube, solid({0.76f, 0.68f, 0.5f, 1.0f}, 0.9f), {0.0f, -1.5f, 0.0f}, {30.0f, 1.0f, 30.0f});
    Entity lake = world->create("Lake");
    lake.setPosition({0.0f, 0.25f, 0.0f});
    lake.add<WaterSurfaceComponent>().size = {40.0f, 40.0f};
    sunAndSky();
    renderFrames(CameraParams::lookAt({0.0f, -0.4f, 3.0f}, {0.0f, -0.6f, -3.0f}, 70.0f), 4);
    EXPECT_TRUE(ranPass("Water.Underwater"));
}
