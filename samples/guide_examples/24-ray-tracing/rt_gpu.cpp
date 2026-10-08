// Глава 24: галочка r.RayTracing на реальном устройстве (headless). На Mac (MoltenVK, без RT) проверяется, что
// галочка инертна: те же проходы и та же картинка; на RTX-машине — что сцена ускоряющих структур строится.
// Без Vulkan-устройства — GTEST_SKIP.
#include "guide_render_scene.hpp"

#include <oxwald/render/features/raytracing/raytracing.hpp>
#include <oxwald/render/features/raytracing/rt_api.hpp>

#include <cstdio>

using namespace ox;
using namespace ox::render;
using guide::CVarScope;

namespace {

class GuideRayTracingGpu : public guide::RenderScene {
protected:
    void scene() {
        const Uuid grey = solid({0.8f, 0.8f, 0.8f, 1.0f});
        mesh(Primitive::Plane, grey, {0, 0, 0}, glm::vec3(30.0f));
        mesh(Primitive::Cube, grey, {-0.8f, 0.5f, 0.0f});
        mesh(Primitive::Sphere, grey, {0.6f, 0.4f, 0.8f}, glm::vec3(0.8f));
        sunAndSky();
    }
    CameraParams cam() const { return CameraParams::lookAt({0.3f, 2.6f, 4.2f}, {0.2f, 0.3f, 0.0f}, 55.0f); }
};

} // namespace

TEST_F(GuideRayTracingGpu, StatusOfThisDevice) {
    // То, что редактор показывает в баннере Project Settings → Rendering и в подсказке у галочки.
    const rt::RtStatus status = rt::rayTracingStatus(device->caps());
    std::printf("[rt] %s: %s\n", device->caps().gpuName.c_str(),
                status.available ? "ray tracing available" : status.reason.c_str());
    for (const rt::RtEffectStatus& e : status.effects) {
        std::printf("[rt]   %-26s %-34s %s\n", e.name.c_str(), e.cvar.c_str(), e.available ? "ok" : "unavailable");
    }
    EXPECT_EQ(status.available, device->caps().rayTracingSupported());
    EXPECT_EQ(status.available, status.reason.empty());
}

TEST_F(GuideRayTracingGpu, CheckboxSwapsEffectsOrIsInert) {
    scene();
    const std::vector<u8> raster = renderFrames(cam(), 3);
    const u32 rasterPasses = renderer->stats().renderGraphPasses;
    rt::RayTracingSceneApi* api = rt::RayTracingSceneApi::find(renderer->features());
    ASSERT_NE(api, nullptr) << "область raytracing регистрируется вместе со встроенными фичами";

    CVarScope on("r.RayTracing", "true");
    const std::vector<u8> traced = renderFrames(cam(), 3);
    if (!device->caps().rayTracingSupported()) {
        // macOS / MoltenVK: рендерер принудительно гасит r.RayTracing, граф и картинка не меняются.
        EXPECT_FALSE(renderer->settings().rayTracing);
        EXPECT_FALSE(api->activeThisFrame());
        EXPECT_EQ(renderer->stats().renderGraphPasses, rasterPasses);
        EXPECT_LT(meanAbsDifference(raster, traced), 0.5);
        return;
    }
    // RTX: TLAS построен, RT-проходы добавились (смена без перезапуска — граф пересобирается каждый кадр).
    EXPECT_TRUE(renderer->settings().rayTracing);
    EXPECT_TRUE(api->activeThisFrame());
    EXPECT_GT(api->tlasInstanceCount(), 0u);
    EXPECT_NE(renderer->stats().renderGraphPasses, rasterPasses);
}
