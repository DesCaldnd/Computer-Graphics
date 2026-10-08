// Глава 18: рендерер в игровом цикле — ox::render::createRenderer() как IRenderer движка (тот же путь, что
// у OxwaldPlayer). Headless: рендер в офскрин-текстуру. Без Vulkan тест пропускается.
#include <oxwald/render/render.hpp>
#include <oxwald/render/runtime_renderer.hpp>
#include <oxwald/runtime/engine.hpp>
#include <oxwald/scene/scene.hpp>

#include <gtest/gtest.h>

#include <filesystem>

TEST(GuideRuntimeRenderer, EngineRendersWithVulkanRenderer) {
    ox::registerSceneTypes();
    const auto userDir = std::filesystem::temp_directory_path() / ("oxwald_guide_rt_" + ox::Uuid::generate().toString());

    ox::Engine engine;
    engine.setRenderer(ox::render::createRenderer({.headlessWidth = 320, .headlessHeight = 180})); // до init()
    ox::EngineConfig config;
    config.headless = true; // без окна: офскрин-цель вместо swapchain
    config.userDir = userDir;
    if (!engine.init(config)) GTEST_SKIP() << "рендерер не инициализировался (нет Vulkan?)";

    ox::World& world = engine.world();
    ox::Entity cam = world.create("Camera");
    cam.setPosition({0, 1, 4});
    cam.add<ox::CameraComponent>().primary = true; // рендер идёт из основной камеры
    ox::Entity cube = world.create("Cube");
    auto& mr = cube.add<ox::MeshRendererComponent>();
    mr.mesh = ox::render::primitiveUuid(ox::render::Primitive::Cube);
    // Пустой список материалов → материал по умолчанию.

    engine.run(3);
    engine.pipeline().flush();

    ox::render::Renderer* r = ox::render::rendererOf(engine.renderer()); // доступ к Renderer за IRenderer
    ASSERT_NE(r, nullptr);
    EXPECT_GE(r->stats().instances, 1u);
    EXPECT_GT(r->stats().drawCalls, 0u);
    engine.shutdown();
    std::filesystem::remove_all(userDir);
}
