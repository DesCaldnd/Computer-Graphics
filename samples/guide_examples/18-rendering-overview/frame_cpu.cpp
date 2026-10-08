// Глава 18: CPU-часть рендерера — точки внедрения и реестр фич, имена и форматы ресурсов, снимок настроек
// (RenderSettings), extract мира в RenderSnapshot, соглашения камеры (docs/guide/18-rendering-overview.md).
// Устройство не нужно.
#include <oxwald/render/render.hpp>
#include <oxwald/scene/scene.hpp>
#include <oxwald/scene/world.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace ox;
using namespace ox::render;

namespace {

// Минимальная фича для экспериментов с реестром: ничего не рисует.
struct NamedFeature final : IRenderFeature {
    std::string n, group;
    InjectionMask mask;
    i32 prio = 0, ord = 0;
    bool needsRt = false;
    NamedFeature(std::string name, InjectionMask m, std::string g = {}, i32 p = 0, i32 o = 0, bool rt = false)
        : n(std::move(name)), group(std::move(g)), mask(m), prio(p), ord(o), needsRt(rt) {}
    std::string_view name() const override { return n; }
    InjectionMask injectionPoints() const override { return mask; }
    std::string_view exclusiveGroup() const override { return group; }
    i32 priority() const override { return prio; }
    i32 order() const override { return ord; }
    bool isEnabled(const RenderSettings& s, const rhi::DeviceCaps& caps) const override {
        return !needsRt || (s.rayTracing && caps.rayTracingSupported()); // типичное условие RT-варианта
    }
    void setup(FeatureContext&) override {}
};

std::vector<std::string> names(const std::vector<IRenderFeature*>& v) {
    std::vector<std::string> out;
    for (auto* f : v) out.emplace_back(f->name());
    return out;
}

} // namespace

TEST(GuideRenderFrame, InjectionPointsAndResourceContracts) {
    // Маска точек внедрения: одна фича может работать в нескольких точках (setup вызывается для каждой).
    const InjectionMask m = maskOf(InjectionPoint::AfterDepth, InjectionPoint::AfterOpaque);
    EXPECT_NE(m & maskOf(InjectionPoint::AfterOpaque), 0u);
    EXPECT_EQ(m & maskOf(InjectionPoint::Lighting), 0u);
    EXPECT_STREQ(injectionPointName(InjectionPoint::BeforePostProcess), "BeforePostProcess");
    EXPECT_EQ(kInjectionPointCount, 12u);

    // Имена ресурсов кадра и их форматы — константы, а не строковые литералы в коде фич.
    EXPECT_EQ(res::kSceneColorHDR, "SceneColorHDR");
    EXPECT_EQ(res::kAO, "AO");
    EXPECT_EQ(formats::kSceneColor, VK_FORMAT_R16G16B16A16_SFLOAT);
    EXPECT_EQ(formats::kDepth, VK_FORMAT_D32_SFLOAT);
    EXPECT_EQ(formats::kAO, VK_FORMAT_R8_UNORM);
    EXPECT_EQ(decodeEntityId(encodeEntityId(41)), 41u);
    EXPECT_EQ(kNoEntity, 0u);
}

TEST(GuideRenderFrame, ExclusiveGroupsOrderAndUpscaleSlot) {
    FeatureRegistry reg;
    reg.emplace<NamedFeature>("MyShadows", maskOf(InjectionPoint::Shadows), "MyShadowsGroup", 0);
    reg.emplace<NamedFeature>("MyShadowsRT", maskOf(InjectionPoint::Shadows), "MyShadowsGroup", 100, 0, true);
    reg.emplace<NamedFeature>("MyBloom", maskOf(InjectionPoint::PostProcess), "", 0, 200);
    reg.emplace<NamedFeature>("MyExposure", maskOf(InjectionPoint::PostProcess), "", 0, 100);
    reg.emplace<NamedFeature>("MyFsr", maskOf(InjectionPoint::Upscale), "", 0);
    reg.emplace<NamedFeature>("MyDlss", maskOf(InjectionPoint::Upscale), "", 10);

    RenderSettings s;
    rhi::DeviceCaps noRt; // например, MoltenVK
    s.rayTracing = true;
    auto resolved = reg.resolve(s, noRt);
    EXPECT_EQ(names(FeatureRegistry::at(resolved, InjectionPoint::Shadows)), std::vector<std::string>{"MyShadows"})
        << "RT-вариант выключен без ray query — работает растровый из той же группы";
    rhi::DeviceCaps rt;
    rt.accelerationStructure = rt.rayQuery = true;
    EXPECT_EQ(names(FeatureRegistry::at(reg.resolve(s, rt), InjectionPoint::Shadows)),
              std::vector<std::string>{"MyShadowsRT"}) << "в группе побеждает включённая фича с большим priority";
    EXPECT_EQ(names(FeatureRegistry::at(resolved, InjectionPoint::PostProcess)),
              (std::vector<std::string>{"MyExposure", "MyBloom"})) << "порядок внутри точки — по order()";
    EXPECT_EQ(names(FeatureRegistry::at(resolved, InjectionPoint::Upscale)), std::vector<std::string>{"MyDlss"})
        << "Upscale — единственный слот";

    // У каждой фичи есть bool-cvar r.Feature.<Name> (по умолчанию true).
    FeatureRegistry::toggle("MyBloom").set(false);
    EXPECT_EQ(names(FeatureRegistry::at(reg.resolve(s, noRt), InjectionPoint::PostProcess)),
              std::vector<std::string>{"MyExposure"});
    CVarRegistry::instance().set("r.Feature.MyBloom", "true", CVarSource::Code);
    EXPECT_EQ(FeatureRegistry::at(reg.resolve(s, noRt), InjectionPoint::PostProcess).size(), 2u);
}

TEST(GuideRenderFrame, SettingsSnapshotFromCVars) {
    registerRenderCVars(); // Renderer::create делает это сам; нужно, если вы читаете настройки без рендерера
    auto& cvars = CVarRegistry::instance();
    cvars.set("r.DebugView", "Normals", CVarSource::Code);
    cvars.set("r.ScreenPercentage", "75", CVarSource::Code);
    const RenderSettings s = RenderSettings::fromCVars(); // снимок раз в кадр: кадр внутренне согласован
    EXPECT_EQ(s.debugView, DebugView::Normals);
    EXPECT_FLOAT_EQ(s.screenPercentage, 75.0f);
    EXPECT_STREQ(debugViewName(s.debugView), "Normals");
    cvars.set("r.DebugView", "None", CVarSource::Code);
    cvars.set("r.ScreenPercentage", "100", CVarSource::Code);
    EXPECT_EQ(RenderSettings::fromCVars().debugView, DebugView::None);
}

TEST(GuideRenderFrame, ExtractWorldIntoSnapshot) {
    registerSceneTypes();
    World world;
    Entity cam = world.create("Camera");
    cam.setPosition({0, 1.7f, 5});
    auto& cc = cam.add<CameraComponent>();
    cc.primary = true;
    cc.verticalFov = 55.0f;

    Entity crate = world.create("Crate");
    auto& mr = crate.add<MeshRendererComponent>();
    mr.mesh = primitiveUuid(Primitive::Cube);              // встроенный примитив, всегда в GPU-кэше
    mr.materials = {Uuid::fromName("Materials/wood.oxmat")}; // один материал на сабмеш
    mr.castShadows = false;

    Entity sun = world.create("Sun");
    sun.add<LightComponent>().type = LightType::Directional;
    world.create("Environment").add<EnvironmentComponent>().sun = sun.ref();

    world.updateTransforms();           // WorldTransform текущего кадра
    world.snapshotPreviousTransforms(); // предыдущий — для motion vectors
    crate.setPosition({1, 0, 0});
    world.updateTransforms();

    // Игровой поток: копия всего, что нужно рендеру. Рендерер мир не трогает.
    SnapshotBuffer buffers;
    RenderSnapshot& out = buffers.writeSlot();
    extract(world, out, {.time = 1.0, .deltaTime = 1.0f / 60.0f, .frame = 60});
    buffers.publish();

    const RenderSnapshot& s = buffers.readSlot(); // поток рендера
    ASSERT_EQ(s.primaryCamera(), 0);
    ASSERT_EQ(s.meshes.size(), 1u);
    const SnapshotMesh& m = s.meshes[0];
    EXPECT_EQ(m.mesh, primitiveUuid(Primitive::Cube));
    EXPECT_EQ(s.materials[m.materialOffset], Uuid::fromName("Materials/wood.oxmat"));
    EXPECT_EQ(m.flags & kMeshCastShadows, 0u);
    EXPECT_NEAR(m.world[3].x, 1.0f, 1e-5f);
    EXPECT_NEAR(m.prevWorld[3].x, 0.0f, 1e-5f);
    ASSERT_TRUE(s.environment.has_value());
    EXPECT_EQ(s.lights[usize(s.environment->sunLight)].light.type, LightType::Directional);

    // Камера рендера из компонента: Y вниз в NDC (Vulkan), reversed-Z (near → 1, far → 0).
    const CameraParams params = CameraParams::fromComponent(s.cameras[0].camera, s.cameras[0].world);
    EXPECT_NEAR(params.verticalFov, glm::radians(55.0f), 1e-5f);
    const CameraParams look = CameraParams::lookAt({0, 0, 0}, {0, 0, -1}, 60.0f, 0.5f, 100.0f);
    const glm::vec4 nearP = look.projectionMatrix(1.0f) * look.viewMatrix() * glm::vec4(0, 0, -0.5f, 1);
    const glm::vec4 farP = look.projectionMatrix(1.0f) * look.viewMatrix() * glm::vec4(0, 0, -100.0f, 1);
    EXPECT_NEAR(nearP.z / nearP.w, 1.0f, 1e-5f);
    EXPECT_NEAR(farP.z / farP.w, 0.0f, 1e-5f);
}
