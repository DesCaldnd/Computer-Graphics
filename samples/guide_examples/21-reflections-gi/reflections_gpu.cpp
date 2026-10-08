// Глава 21: отражения и GI на GPU — проба отражений + SSR, бейк и сохранение .oxcube, планарное зеркало, GTAO,
// объём проб освещённости (docs/guide/21-reflections-gi.md). Без Vulkan-устройства тесты пропускаются.
#include "../20-lighting-shadows/guide_render_scene.hpp"

#include <oxwald/core/scalability.hpp>
#include <oxwald/render/features/reflections/reflections.hpp>

#include <filesystem>

using namespace ox;
using namespace ox::render;
using guide::Image;
using guide::ScopedCVar;

namespace {

class GuideReflectionsGpu : public guide::GuideRenderTest {
protected:
    // Хромированная сфера на глянцевом полу, цветные кубы вокруг, солнце и небо.
    void showroom() {
        mesh(Primitive::Plane, material({0.5f, 0.5f, 0.5f, 1.0f}, 0.0f, 0.15f), {0, 0, 0}, glm::vec3(20.0f));
        mesh(Primitive::Sphere, material({0.95f, 0.95f, 0.95f, 1.0f}, 1.0f, 0.05f), {0.0f, 1.0f, 0.0f});
        mesh(Primitive::Cube, material({0.85f, 0.1f, 0.1f, 1.0f}, 0.0f, 0.6f), {0.0f, 1.0f, -3.0f}, glm::vec3(2.0f));
        mesh(Primitive::Cube, material({0.1f, 0.25f, 0.9f, 1.0f}, 0.0f, 0.6f), {3.0f, 1.0f, 0.5f}, glm::vec3(1.5f, 2.0f, 1.5f));
        Entity sun = world->create("Sun");
        sun.setRotation(lookRotation(glm::normalize(glm::vec3(-0.4f, -0.8f, -0.45f))));
        auto& l = sun.add<LightComponent>();
        l.type = LightType::Directional;
        l.intensity = 20000.0f;
        world->create("Environment").add<EnvironmentComponent>();
    }
    Entity probe(ReflectionProbeUpdate mode = ReflectionProbeUpdate::Baked) {
        Entity e = world->create("ReflectionProbe");
        e.setPosition({0.0f, 1.0f, 0.0f});
        auto& p = e.add<ReflectionProbeComponent>();
        p.extents = {9.0f, 5.0f, 9.0f};
        p.update = mode;
        return e;
    }
    static CameraParams showroomCamera() { return camera({0.0f, 1.5f, 4.4f}, {0.0f, 0.8f, 0.0f}, 13.0f); }
};

} // namespace

TEST_F(GuideReflectionsGpu, ProbeAndSsr) {
    showroom();
    probe();
    ScopedCVar ssr("r.SSR", "true");          // по умолчанию выключен: включают пресеты Medium+
    ScopedCVar half("r.SSR.HalfRes", "false");
    const Image withSsr = render(showroomCamera(), 128, 128, 8); // SSR накапливается во времени
    EXPECT_TRUE(ranPass("Reflections.SSRTrace"));
    ScopedCVar off("r.SSR", "false");
    const Image probeOnly = render(showroomCamera(), 128, 128, 4);
    EXPECT_GT(guide::meanDifference(withSsr, probeOnly), 0.2); // SSR добавляет отражения объектов на экране
}

TEST_F(GuideReflectionsGpu, BakeAndSaveProbe) {
    showroom();
    Entity p = probe(ReflectionProbeUpdate::Baked);
    const Uuid id = p.get<IdComponent>().id;

    // Кнопка «Bake probes» редактора делает то же самое.
    reflections::requestBake(*renderer);
    for (int i = 0; i < 16 && reflections::bakeInProgress(*renderer); ++i) render(showroomCamera(), 64, 64, 1);
    ASSERT_FALSE(reflections::bakeInProgress(*renderer));

    auto baked = reflections::readBakedProbes(*renderer); // ждёт GPU: вызывать между кадрами
    ASSERT_EQ(baked.size(), 1u);
    EXPECT_EQ(baked[0].first, id);
    const auto file = std::filesystem::temp_directory_path() / "oxwald_guide_reflections" / "showroom.oxcube";
    ASSERT_TRUE(reflections::saveOxCube(file, baked[0].second));

    // При загрузке сцены: прочитать файл и отдать рендеру — Baked-проба с этим Uuid не будет сниматься заново.
    Result<reflections::BakedCubemap> loaded = reflections::loadOxCube(file);
    ASSERT_TRUE(loaded);
    reflections::setBakedProbe(*renderer, id, std::move(*loaded));
    EXPECT_GT(render(showroomCamera(), 64, 64, 2).meanLuminance(), 0.05f);
}

TEST_F(GuideReflectionsGpu, PlanarMirror) {
    showroom();
    Entity mirror = mesh(Primitive::Plane, material({0.95f, 0.95f, 0.95f, 1.0f}, 1.0f, 0.02f), {0, 0.01f, 2.0f},
                         glm::vec3(3.0f));
    mirror.add<PlanarReflectorComponent>().size = {0.5f, 0.5f}; // в локальных единицах плоскости (масштаб 3 → 1.5 м)
    const CameraParams cam = camera({0.0f, 2.2f, 6.0f}, {0.0f, 0.4f, 0.0f}, 13.0f);
    const Image img = render(cam, 128, 128, 4);
    EXPECT_TRUE(ranPass("Reflections.Planar"));
    ScopedCVar off("r.PlanarReflections", "false");
    EXPECT_GT(guide::meanDifference(img, render(cam, 128, 128, 2)), 0.2);
}

TEST_F(GuideReflectionsGpu, GtaoDarkensCorners) {
    const Uuid white = material({0.85f, 0.85f, 0.85f, 1.0f}, 0.0f, 0.8f);
    mesh(Primitive::Plane, white, {0, 0, 0}, glm::vec3(20.0f));
    mesh(Primitive::Cube, white, {-2.0f, 1.5f, 0.0f}, glm::vec3(0.2f, 3.0f, 6.0f));
    mesh(Primitive::Cube, white, {0.0f, 1.5f, -2.0f}, glm::vec3(6.0f, 3.0f, 0.2f));
    mesh(Primitive::Sphere, white, {0.6f, 0.5f, 0.2f});
    world->create("Environment").add<EnvironmentComponent>(); // только небо: AO хорошо видно
    const CameraParams cam = camera({2.5f, 2.6f, 3.2f}, {-0.6f, 0.4f, -0.6f}, 11.0f, 55.0f);

    ScopedCVar method("r.AO.Method", "2"); // 0 выкл, 1 SSAO, 2 GTAO
    ScopedCVar quality("r.AO.Quality", "2");
    const Image withAo = render(cam, 128, 128, 8);
    EXPECT_TRUE(ranPass("AO.Trace"));
    ScopedCVar off("r.AO.Method", "0");
    const Image noAo = render(cam, 128, 128, 2);
    EXPECT_LT(withAo.meanLuminance(), noAo.meanLuminance()); // AO только затемняет
}

TEST_F(GuideReflectionsGpu, IrradianceVolumeBakesOverFrames) {
    const Uuid white = material({0.85f, 0.85f, 0.85f, 1.0f}, 0.0f, 0.9f);
    mesh(Primitive::Plane, white, {0, 0, 0}, glm::vec3(30.0f));
    mesh(Primitive::Cube, material({0.9f, 0.05f, 0.05f, 1.0f}, 0.0f, 0.9f), {-2.0f, 1.5f, 0.0f}, glm::vec3(0.3f, 3.0f, 6.0f));
    Entity sun = world->create("Sun");
    sun.setRotation(lookRotation(glm::normalize(glm::vec3(-0.9f, -0.3f, 0.25f))));
    auto& l = sun.add<LightComponent>();
    l.type = LightType::Directional;
    l.intensity = 20000.0f;
    world->create("Environment").add<EnvironmentComponent>().ambientIntensity = 0.4f;
    const CameraParams cam = camera({3.5f, 2.5f, 4.0f}, {-1.0f, 0.5f, 0.0f}, 13.0f, 55.0f);
    const Image noGi = render(cam, 128, 128, 2);

    Entity v = world->create("IrradianceVolume");
    v.setPosition({0.0f, 1.5f, 0.0f});
    auto& vol = v.add<IrradianceVolumeComponent>();
    vol.extents = {3.0f, 1.5f, 3.0f};
    vol.probeCount = {6, 3, 6};    // 108 проб
    vol.captureResolution = 16;    // грань куба при съёмке пробы
    ScopedCVar perFrame("r.GI.IrradianceVolumes.ProbesPerFrame", "64"); // бюджет бейка в кадр
    const Image gi = render(cam, 128, 128, 6);
    EXPECT_FALSE(reflections::bakeInProgress(*renderer));
    EXPECT_GT(guide::meanDifference(gi, noGi), 0.2); // отражённый красный свет на полу
}
