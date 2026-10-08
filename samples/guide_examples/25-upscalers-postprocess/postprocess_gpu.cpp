// Глава 25: апскейлеры и постобработка на настоящем рендерере (headless). Без Vulkan-устройства — GTEST_SKIP.
// Проверяет разрешение рендера при TAAU, откат DLSS → TAAU там, где DLSS нет, и bloom из PostProcessVolume.
#include "guide_render_scene.hpp"

#include <oxwald/render/components/postprocess.hpp>
#include <oxwald/render/features/postprocess/postprocess.hpp>

#include <cstdio>

using namespace ox;
using namespace ox::render;
using guide::CVarScope;

namespace {

class GuidePostProcessGpu : public guide::RenderScene {
protected:
    void scene() {
        mesh(Primitive::Plane, solid({0.5f, 0.5f, 0.5f, 1.0f}, 0.8f), {0, 0, 0}, glm::vec3(30.0f));
        mesh(Primitive::Sphere, solid({0.9f, 0.1f, 0.1f, 1.0f}, 0.4f), {-1.0f, 0.5f, 0.0f});
        mesh(Primitive::Sphere, solid({0.1f, 0.2f, 0.9f, 1.0f}, 0.4f), {1.0f, 0.5f, 0.0f});
        sunAndSky();
    }
    CameraParams cam() const { return CameraParams::lookAt({0.0f, 2.0f, 5.0f}, {0.0f, 0.4f, 0.0f}, 50.0f); }
};

} // namespace

TEST_F(GuidePostProcessGpu, TaauRendersAtHalfResolution) {
    scene();
    CVarScope up("r.Upscaler", "TAAU");
    CVarScope q("r.Upscaler.Quality", "Performance"); // 50 %
    renderFrames(cam(), 8, 1.0f / 30.0f, 256, 256);
    const RenderView* v = renderer->view(view);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->outputExtent().width, 256u);
    EXPECT_EQ(v->renderExtent().width, 128u);  // сцена рисуется в 128×128…
    EXPECT_TRUE(ranPass("TAAU"));              // …и TAAU восстанавливает 256×256
    EXPECT_FALSE(ranPass("FSR1"));
}

TEST_F(GuidePostProcessGpu, DlssFallsBackToTaau) {
    const UpscalerAvailability dlss = upscalerAvailability(UpscalerType::DLSS, device.get());
    std::printf("[dlss] %s\n", dlss.available ? "available" : dlss.reason.c_str());
    if (dlss.available) GTEST_SKIP() << "DLSS доступен — путь отката здесь не проверить";
    scene();
    CVarScope up("r.Upscaler", "DLSS");
    CVarScope q("r.Upscaler.Quality", "Quality");
    renderFrames(cam(), 4);
    EXPECT_TRUE(ranPass("TAAU")) << "r.Upscaler=DLSS без DLSS работает как TAAU";
}

TEST_F(GuidePostProcessGpu, BloomFromVolume) {
    scene();
    // Яркий излучающий шар — источник свечения.
    assets::MaterialAsset lamp;
    lamp.baseColor = {0.0f, 0.0f, 0.0f, 1.0f};
    lamp.emissive = {30.0f, 12.0f, 3.0f};
    mesh(Primitive::Sphere, addMaterial(lamp), {0.0f, 1.2f, 0.0f}, glm::vec3(0.3f));

    CVarScope bloomOn("r.Bloom", "true");   // фича включена (обычно — уровнем PostProcess)…
    const std::vector<u8> mild = renderFrames(cam(), 3);
    EXPECT_TRUE(ranPass("Bloom"));

    // …а сила — из объёма постобработки.
    Entity volume = world->create("PostProcessVolume");
    auto& v = volume.add<PostProcessVolumeComponent>();
    v.settings.overrideBloom = true;
    v.settings.bloomIntensity = 0.5f;
    const std::vector<u8> strong = renderFrames(cam(), 3);
    EXPECT_GT(meanAbsDifference(mild, strong), 0.5) << "объём меняет силу bloom";
}
