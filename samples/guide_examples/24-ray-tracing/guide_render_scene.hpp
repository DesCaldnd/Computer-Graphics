#pragma once

// Небольшой headless-стенд для GPU-примеров глав 23–25: устройство с validation layers, Renderer, мир ECS,
// offscreen-цель и цикл «extract → beginFrame → renderView → endFrame». Без Vulkan-устройства тест пропускается.

#include <oxwald/core/cvar.hpp>
#include <oxwald/render/render.hpp>
#include <oxwald/rhi/device.hpp>
#include <oxwald/scene/scene.hpp>
#include <oxwald/scene/world.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>

namespace guide {

using ox::f32;
using ox::f64;
using ox::u8;
using ox::u32;
using ox::u64;
using ox::usize;

// Временно меняет cvar и возвращает старое значение в деструкторе.
class CVarScope {
public:
    CVarScope(std::string name, std::string_view value) : m_name(std::move(name)) {
        if (ox::ICVar* c = ox::CVarRegistry::instance().find(m_name)) m_previous = c->toString();
        ox::CVarRegistry::instance().set(m_name, value, ox::CVarSource::Code);
    }
    ~CVarScope() {
        if (!m_previous.empty()) ox::CVarRegistry::instance().set(m_name, m_previous, ox::CVarSource::Code);
    }
    CVarScope(const CVarScope&) = delete;
    CVarScope& operator=(const CVarScope&) = delete;

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
        renderer = ox::render::Renderer::create(*device);
        world = std::make_unique<ox::World>();
    }
    void TearDown() override {
        if (!device) return;
        device->waitIdle();
        if (view) renderer->destroyView(view);
        renderer.reset();
        if (target) device->destroy(target);
        world.reset();
        device.reset();
        EXPECT_EQ(ox::rhi::Device::validationErrorCount(), 0u) << "validation layer сообщил об ошибках";
    }

    ox::Uuid addMaterial(const ox::assets::MaterialAsset& m) {
        const ox::Uuid id = ox::Uuid::fromName(std::format("guide.material.{}", m_materials++));
        renderer->resources().addMaterial(id, m);
        return id;
    }
    ox::Uuid solid(glm::vec4 color, f32 roughness = 0.6f) {
        ox::assets::MaterialAsset m;
        m.baseColor = color;
        m.roughness = roughness;
        return addMaterial(m);
    }
    ox::Entity mesh(ox::render::Primitive p, const ox::Uuid& material, glm::vec3 position,
                    glm::vec3 scale = glm::vec3(1.0f)) {
        ox::Entity e = world->create(ox::render::primitiveName(p));
        e.setPosition(position);
        e.setScale(scale);
        auto& mr = e.add<ox::MeshRendererComponent>();
        mr.mesh = ox::render::primitiveUuid(p);
        mr.materials = {material};
        return e;
    }
    void sunAndSky() {
        ox::Entity sun = world->create("Sun");
        sun.setRotation(ox::lookRotation(glm::normalize(glm::vec3(-0.4f, -0.8f, -0.45f)), ox::kWorldUp));
        auto& l = sun.add<ox::LightComponent>();
        l.type = ox::LightType::Directional;
        l.intensity = 30000.0f;
        l.castShadows = true;
        world->create("Environment").add<ox::EnvironmentComponent>();
    }

    // Рендерит `frames` кадров с шагом dt (частицам нужно время) и возвращает RGBA8 последнего кадра.
    std::vector<u8> renderFrames(const ox::render::CameraParams& camera, u32 frames, f32 dt = 1.0f / 30.0f,
                                 u32 width = 256, u32 height = 256) {
        if (!target) {
            ox::rhi::TextureDesc d;
            d.name = "guide.target";
            d.format = VK_FORMAT_R8G8B8A8_UNORM;
            d.width = width;
            d.height = height;
            d.usage = ox::rhi::TextureUsage::ColorAttachment | ox::rhi::TextureUsage::Sampled |
                      ox::rhi::TextureUsage::TransferSrc;
            target = device->createTexture(d);
        }
        if (!view) view = renderer->createView({.name = "GuideView"});
        renderer->resources().flush();
        for (u32 f = 0; f < frames; ++f) {
            world->updateTransforms();
            world->snapshotPreviousTransforms();
            ox::render::extract(*world, snapshot, {.time = f64(f) * dt, .deltaTime = dt});
            device->beginFrame();
            renderer->beginFrame(snapshot);
            ox::render::ViewRenderRequest req;
            req.view = view;
            req.camera = camera;
            req.target.texture = target;
            req.target.finalAccess = ox::rhi::Access::TransferRead;
            renderer->renderView(req);
            renderer->endFrame();
            device->endFrame();
        }
        device->waitIdle();
        return device->readTexture(target);
    }

    // Был ли в последнем завершённом кадре проход, имя которого содержит `part`.
    bool ranPass(std::string_view part) const {
        for (const ox::render::PassTiming& p : renderer->stats().passes) {
            if (p.name.find(part) != std::string::npos) return true;
        }
        return false;
    }

    static f64 meanAbsDifference(const std::vector<u8>& a, const std::vector<u8>& b) {
        u64 sum = 0, n = 0;
        for (usize i = 0; i < a.size() && i < b.size(); ++i) {
            if (i % 4 == 3) continue;
            sum += u64(std::abs(int(a[i]) - int(b[i])));
            ++n;
        }
        return n ? f64(sum) / f64(n) : 0.0;
    }

    std::unique_ptr<ox::rhi::Device> device;
    std::unique_ptr<ox::render::Renderer> renderer;
    std::unique_ptr<ox::World> world;
    ox::render::RenderSnapshot snapshot;
    ox::rhi::TextureHandle target;
    ox::render::ViewId view = 0;

private:
    u32 m_materials = 0;
};

} // namespace guide
