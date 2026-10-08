// Глава 21: компоненты отражений и GI на сцене, извлечение в снимок, форматы .oxcube/.oxirr, cvar'ы и уровни
// качества групп Reflections и GlobalIllumination (docs/guide/21-reflections-gi.md). GPU не нужен.
#include <oxwald/core/cvar.hpp>
#include <oxwald/core/scalability.hpp>
#include <oxwald/render/features/reflections/reflections.hpp>
#include <oxwald/render/register_types.hpp>
#include <oxwald/scene/scene.hpp>
#include <oxwald/scene/world.hpp>

#include <gtest/gtest.h>

#include <filesystem>

using namespace ox;
using namespace ox::render;

TEST(GuideReflections, ComponentsOnTheScene) {
    registerSceneTypes();
    registerRenderTypes(); // компоненты рендера + extract hooks (рантайм и редактор делают это сами)

    World world;
    // Проба отражений в комнате 8 × 4 × 8 м: коробка влияния = стены, box projection включён.
    Entity room = world.create("RoomProbe");
    room.setPosition({0.0f, 2.0f, 0.0f});
    auto& probe = room.add<ReflectionProbeComponent>();
    probe.extents = {4.0f, 2.0f, 4.0f};          // полуразмеры коробки (м)
    probe.blendDistance = 0.5f;                  // плавный выход за коробку
    probe.boxProjection = true;                  // параллакс-коррекция под стены комнаты
    probe.captureOffset = {0.0f, -0.5f, 0.0f};   // точка съёмки ниже центра — на высоте глаз
    probe.update = ReflectionProbeUpdate::Baked; // снять один раз (или загрузить .oxcube)
    probe.priority = 1;                          // перекрывает «уличную» пробу

    // Зеркало на полу: плоскость = локальная XZ сущности, нормаль +Y.
    Entity mirror = world.create("Mirror");
    auto& planar = mirror.add<PlanarReflectorComponent>();
    planar.size = {2.0f, 1.0f};   // полуразмеры в XZ (0 = бесконечная плоскость)
    planar.maxRoughness = 0.3f;   // шероховатее — отражение берётся из проб/SSR

    // Сетка проб освещённости на всю комнату: 8 × 4 × 8 = 256 проб.
    Entity gi = world.create("RoomGI");
    gi.setPosition({0.0f, 2.0f, 0.0f});
    auto& volume = gi.add<IrradianceVolumeComponent>();
    volume.extents = {4.0f, 2.0f, 4.0f};
    volume.probeCount = {8, 4, 8};

    world.updateTransforms();
    RenderSnapshot snap;
    extract(world, snap);
    const auto* ext = snap.findExtension<reflections::ReflectionSnapshot>();
    ASSERT_NE(ext, nullptr);
    ASSERT_EQ(ext->probes.size(), 1u);
    EXPECT_TRUE(ext->probes[0].uuid.isValid()); // ключ запечённых данных — Uuid сущности
    EXPECT_EQ(ext->planars.size(), 1u);
    ASSERT_EQ(ext->volumes.size(), 1u);
    EXPECT_EQ(ext->volumes[0].volume.probeCount, glm::ivec3(8, 4, 8));
}

TEST(GuideReflections, BakedDataFiles) {
    // .oxcube: префильтрованный куб (RGBA16F, мипы × 6 граней). Здесь — синтетический 8², 2 мипа.
    reflections::BakedCubemap cube;
    cube.size = 8;
    cube.mips = 2;
    cube.data.resize((8 * 8 + 4 * 4) * 6 * 8); // texels × 6 граней × 8 байт (RGBA16F)
    const auto dir = std::filesystem::temp_directory_path() / "oxwald_guide_reflections";
    ASSERT_TRUE(reflections::saveOxCube(dir / "room.oxcube", cube));
    Result<reflections::BakedCubemap> loaded = reflections::loadOxCube(dir / "room.oxcube");
    ASSERT_TRUE(loaded) << loaded.error().message;
    EXPECT_EQ(loaded->mips, 2u);

    // .oxirr: SH L1 на пробу + тайлы моментов глубины (10×10 RGBA16F на пробу).
    reflections::BakedIrradianceVolume vol;
    vol.probeCount = {2, 2, 2};
    vol.probes.resize(8);
    vol.moments.assign(8 * 10 * 10 * 8, 0);
    ASSERT_TRUE(reflections::saveOxIrradiance(dir / "room.oxirr", vol));
    EXPECT_TRUE(reflections::loadOxIrradiance(dir / "room.oxirr"));
}

TEST(GuideReflections, ScalabilityLevels) {
    registerReflectionCVars();
    auto& reg = CVarRegistry::instance();

    // По умолчанию (никакой уровень не выбран) SSR и AO выключены — их включает любой пресет.
    EXPECT_EQ(reg.find("r.SSR")->toString(), "false");
    EXPECT_EQ(reg.find("r.AO.Method")->toString(), "0");

    scalability::setGroup(Scalability::Reflections, QualityLevel::Low);
    scalability::setGroup(Scalability::GlobalIllumination, QualityLevel::Low);
    EXPECT_EQ(reg.find("r.SSR")->toString(), "false");            // Low: только пробы
    EXPECT_EQ(reg.find("r.PlanarReflections")->toString(), "false");
    EXPECT_EQ(reg.find("r.AO.Method")->toString(), "1");          // Low: SSAO
    EXPECT_EQ(reg.find("r.GI.IrradianceVolumes")->toString(), "false");

    scalability::setGroup(Scalability::Reflections, QualityLevel::High);
    scalability::setGroup(Scalability::GlobalIllumination, QualityLevel::High);
    EXPECT_EQ(reg.find("r.SSR")->toString(), "true");
    EXPECT_EQ(reg.find("r.SSR.MaxSteps")->toString(), "64");
    EXPECT_EQ(reg.find("r.AO.Method")->toString(), "2");          // GTAO
    EXPECT_EQ(reg.find("r.AO.HalfRes")->toString(), "false");

    // Вернуть объявленные значения (как делают тесты движка).
    for (Scalability g : {Scalability::Reflections, Scalability::GlobalIllumination}) {
        if (ICVar* sg = reg.find("sg." + std::string(scalability::groupName(g)))) sg->reset();
        for (ICVar* c : reg.inGroup(g)) c->reset();
    }
}
