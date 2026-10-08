// Маленький headless-стенд рендера для примеров главы 27: устройство без окна (тесты пропускаются без Vulkan),
// Renderer, сцена через настоящий ECS + extract, offscreen-цель и чтение статистики.
#pragma once

#include <oxwald/core/cvar.hpp>
#include <oxwald/core/jobs.hpp>
#include <oxwald/core/math.hpp>
#include <oxwald/render/render.hpp>
#include <oxwald/rhi/device.hpp>
#include <oxwald/scene/scene.hpp>
#include <oxwald/scene/world.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <format>
#include <memory>
#include <string>

namespace guide {

// Временная правка cvar'а (как CVarScope в тестах рендера).
class CVarOverride {
public:
    CVarOverride(std::string name, const std::string& value) : m_name(std::move(name)) {
        if (ox::ICVar* c = ox::CVarRegistry::instance().find(m_name)) m_previous = c->toString();
        ox::CVarRegistry::instance().set(m_name, value, ox::CVarSource::Code);
    }
    ~CVarOverride() {
        if (!m_previous.empty()) ox::CVarRegistry::instance().set(m_name, m_previous, ox::CVarSource::Code);
    }

private:
    std::string m_name, m_previous;
};

class RenderScene : public ::testing::Test {
protected:
    void SetUp() override {
        ox::registerSceneTypes();
        ox::rhi::Device::resetValidationCounters();
        ox::rhi::DeviceDesc desc;
        desc.appName = "guide_render";
        desc.validation = true;
        desc.shaderOptions.cacheDirectory = std::filesystem::temp_directory_path() / "oxwald_guide_shader_cache";
        std::string error;
        device = ox::rhi::Device::create(desc, &error);
        if (!device) GTEST_SKIP() << "нет Vulkan-устройства: " << error;
        createRenderer({});
        world = std::make_unique<ox::World>();
    }
    void TearDown() override {
        if (!device) return;
        device->waitIdle();
        renderer.reset();
        if (target) device->destroy(target);
        world.reset();
        device.reset();
        EXPECT_EQ(ox::rhi::Device::validationErrorCount(), 0u);
    }
    void createRenderer(const ox::render::RendererDesc& rd) {
        if (renderer) {
            device->waitIdle();
            renderer.reset();
        }
        view = 0;
        renderer = ox::render::Renderer::create(*device, rd);
    }

    ox::Uuid material(glm::vec4 color, ox::f32 roughness = 0.5f) {
        ox::assets::MaterialAsset m;
        m.baseColor = color;
        m.roughness = roughness;
        const ox::Uuid id = ox::Uuid::fromName(std::format("guide.material.{}", materialCounter++));
        renderer->resources().addMaterial(id, m);
        return id;
    }
    ox::Entity mesh(ox::render::Primitive p, const ox::Uuid& mat, glm::vec3 position, glm::vec3 scale = glm::vec3(1.0f)) {
        ox::Entity e = world->create(ox::render::primitiveName(p));
        e.setPosition(position);
        e.setScale(scale);
        auto& mr = e.add<ox::MeshRendererComponent>();
        mr.mesh = ox::render::primitiveUuid(p);
        mr.materials = {mat};
        return e;
    }
    void sunAndSky(bool shadows = true) {
        ox::Entity sun = world->create("Sun");
        sun.setRotation(ox::lookRotation(glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f)), ox::kWorldUp));
        auto& l = sun.add<ox::LightComponent>();
        l.type = ox::LightType::Directional;
        l.intensity = 20000.0f;
        l.castShadows = shadows;
        world->create("Environment").add<ox::EnvironmentComponent>();
    }
    static ox::render::CameraParams camera(glm::vec3 eye, glm::vec3 at) {
        ox::render::CameraParams c = ox::render::CameraParams::lookAt(eye, at, 60.0f, 0.1f, 300.0f);
        c.ev100 = 12.5f;
        return c;
    }

    // Extract (игровой поток) + `frames` кадров рендера (поток рендера) в offscreen-текстуру.
    void render(const ox::render::CameraParams& cam, ox::u32 width, ox::u32 height, ox::u32 frames,
                ox::JobSystem* jobs = nullptr) {
        if (!target || device->desc(target).width != width || device->desc(target).height != height) {
            if (target) device->destroy(target);
            ox::rhi::TextureDesc d;
            d.name = "guide.target";
            d.format = VK_FORMAT_R8G8B8A8_UNORM;
            d.width = width;
            d.height = height;
            d.usage = ox::rhi::TextureUsage::ColorAttachment | ox::rhi::TextureUsage::Sampled |
                      ox::rhi::TextureUsage::TransferSrc;
            target = device->createTexture(d);
        }
        if (!view) view = renderer->createView({.name = "Main"});
        world->updateTransforms();
        world->snapshotPreviousTransforms();
        ox::render::extract(*world, snapshot, {.jobs = jobs});
        renderer->resources().flush();
        for (ox::u32 f = 0; f < frames; ++f) {
            device->beginFrame();
            renderer->beginFrame(snapshot);
            ox::render::ViewRenderRequest req;
            req.view = view;
            req.camera = cam;
            req.target.texture = target;
            req.target.finalAccess = ox::rhi::Access::TransferRead;
            renderer->renderView(req);
            renderer->endFrame();
            device->endFrame();
        }
        device->waitIdle();
    }

    std::unique_ptr<ox::rhi::Device> device;
    std::unique_ptr<ox::render::Renderer> renderer;
    std::unique_ptr<ox::World> world;
    ox::render::RenderSnapshot snapshot;
    ox::rhi::TextureHandle target;
    ox::render::ViewId view = 0;
    ox::u32 materialCounter = 0;
};

} // namespace guide
