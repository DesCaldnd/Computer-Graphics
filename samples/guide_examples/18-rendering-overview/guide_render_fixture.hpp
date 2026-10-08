#pragma once

// Общая фикстура GPU-примеров глав 18 (рендерер) и 19 (материалы): headless-устройство с validation layers,
// Renderer, мир ECS, помощники для сцены и офскрин-рендер с чтением пикселей. Без Vulkan тесты пропускаются.
// Это упрощённая версия engine/render/tests/gpu/render_fixture.hpp (без golden-картинок).

#include <oxwald/core/cvar.hpp>
#include <oxwald/render/render.hpp>
#include <oxwald/rhi/device.hpp>
#include <oxwald/scene/scene.hpp>
#include <oxwald/scene/world.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <vector>

namespace guide {

using namespace ox;
using namespace ox::render;
namespace fs = std::filesystem;

struct Image {
    u32 width = 0, height = 0;
    std::vector<u8> rgba;
    [[nodiscard]] glm::u8vec4 at(u32 x, u32 y) const {
        const usize i = (usize(y) * width + x) * 4;
        return {rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
    }
    [[nodiscard]] f32 luminance(u32 x, u32 y) const {
        const glm::u8vec4 c = at(x, y);
        return (0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b) / 255.0f;
    }
    // Средняя яркость квадрата (2r+1)² вокруг пикселя.
    [[nodiscard]] f32 meanLuminance(glm::ivec2 c, int r) const {
        f32 sum = 0.0f;
        int n = 0;
        for (int y = c.y - r; y <= c.y + r; ++y)
            for (int x = c.x - r; x <= c.x + r; ++x) {
                if (x < 0 || y < 0 || x >= int(width) || y >= int(height)) continue;
                sum += luminance(u32(x), u32(y));
                ++n;
            }
        return n ? sum / f32(n) : 0.0f;
    }
};

// Временное значение cvar'а: восстанавливает прежнее в деструкторе.
class CVarScope {
public:
    CVarScope(std::string name, const std::string& value) : m_name(std::move(name)) {
        if (ICVar* c = CVarRegistry::instance().find(m_name)) m_previous = c->toString();
        CVarRegistry::instance().set(m_name, value, CVarSource::Code);
    }
    ~CVarScope() {
        if (!m_previous.empty()) CVarRegistry::instance().set(m_name, m_previous, CVarSource::Code);
    }
    CVarScope(const CVarScope&) = delete;
    CVarScope& operator=(const CVarScope&) = delete;

private:
    std::string m_name, m_previous;
};

class GuideRenderTest : public ::testing::Test {
protected:
    void SetUp() override {
        registerSceneTypes();
        rhi::Device::resetValidationCounters();
        if (!createDevice({fs::path(OX_GUIDE_DIR) / "shaders"})) GTEST_SKIP() << "нет Vulkan-устройства: " << m_error;
        world = std::make_unique<World>();
    }
    void TearDown() override {
        destroyDevice();
        if (m_created) EXPECT_EQ(rhi::Device::validationErrorCount(), 0u) << "validation layer сообщил об ошибках";
    }

    // Устройство + Renderer. `shaderRoots` — свои каталоги шейдеров игры (ищутся до engine/shaders).
    bool createDevice(std::vector<fs::path> shaderRoots) {
        rhi::DeviceDesc desc;
        desc.appName = "guide_render";
        desc.validation = true;
        desc.shaderOptions.includeRoots = std::move(shaderRoots);
        desc.shaderOptions.cacheDirectory = fs::temp_directory_path() / "oxwald_guide_render_shader_cache";
        device = rhi::Device::create(desc, &m_error);
        if (!device) return false;
        m_created = true;
        renderer = Renderer::create(*device);
        return true;
    }
    void destroyDevice() {
        if (!device) return;
        device->waitIdle();
        renderer.reset(); // Renderer и его ресурсы — до устройства
        if (target) device->destroy(target);
        target = {};
        view = 0;
        device.reset();
    }

    // --- сцена ---
    Uuid addMaterial(const assets::MaterialAsset& m) {
        const Uuid id = Uuid::fromName(std::format("guide.material.{}", m_materialCounter++));
        renderer->resources().addMaterial(id, m);
        return id;
    }
    Uuid material(glm::vec4 baseColor, f32 metallic = 0.0f, f32 roughness = 0.5f) {
        assets::MaterialAsset m;
        m.baseColor = baseColor;
        m.metallic = metallic;
        m.roughness = roughness;
        return addMaterial(m);
    }
    Entity mesh(Primitive p, const Uuid& mat, glm::vec3 position, glm::vec3 scale = glm::vec3(1.0f)) {
        Entity e = world->create(primitiveName(p));
        e.setPosition(position);
        e.setScale(scale);
        auto& mr = e.add<MeshRendererComponent>();
        mr.mesh = primitiveUuid(p);
        mr.materials = {mat};
        return e;
    }
    Entity sun(glm::vec3 direction, f32 lux = 20000.0f) {
        Entity e = world->create("Sun");
        e.setRotation(lookRotation(glm::normalize(direction), kWorldUp));
        auto& l = e.add<LightComponent>();
        l.type = LightType::Directional;
        l.intensity = lux;
        return e;
    }
    Entity environment(f32 skyIntensity = 1.0f, f32 ambient = 1.0f) {
        Entity e = world->create("Environment");
        auto& env = e.add<EnvironmentComponent>();
        env.skyIntensity = skyIntensity;
        env.ambientIntensity = ambient;
        return e;
    }
    static CameraParams camera(glm::vec3 eye, glm::vec3 at, f32 ev100) {
        CameraParams c = CameraParams::lookAt(eye, at, 50.0f, 0.1f, 200.0f);
        c.ev100 = ev100;
        return c;
    }
    // Пиксель вывода, в который проецируется точка мира.
    static glm::ivec2 project(const CameraParams& cam, glm::vec3 p, u32 w, u32 h) {
        const glm::vec4 c = cam.projectionMatrix(f32(w) / f32(h)) * cam.viewMatrix() * glm::vec4(p, 1.0f);
        const glm::vec2 uv = glm::vec2(c) / c.w * 0.5f + 0.5f;
        return {int(uv.x * f32(w)), int(uv.y * f32(h))};
    }

    // --- кадр: extract → beginFrame → renderView → endFrame, чтение последнего кадра ---
    // 4 кадра: GPU-тайминги в stats() отстают на число кадров в полёте, так ranPass() видит этот вызов.
    Image render(const CameraParams& cam, u32 width = 256, u32 height = 256, u32 frames = 4) {
        if (!target || device->desc(target).width != width || device->desc(target).height != height) {
            if (target) device->destroy(target);
            rhi::TextureDesc d;
            d.name = "guide.target";
            d.format = VK_FORMAT_R8G8B8A8_UNORM;
            d.width = width;
            d.height = height;
            d.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferSrc;
            target = device->createTexture(d);
        }
        if (!view) view = renderer->createView({.name = "GuideView"});
        world->updateTransforms();
        world->snapshotPreviousTransforms();
        extract(*world, snapshot);
        renderer->resources().flush(); // дождаться загрузки мешей/материалов (тесты, экраны загрузки)
        for (u32 f = 0; f < frames; ++f) {
            device->beginFrame();
            renderer->beginFrame(snapshot);
            ViewRenderRequest req;
            req.view = view;
            req.camera = cam;
            req.target.texture = target;
            req.target.finalAccess = rhi::Access::TransferRead;
            renderer->renderView(req);
            renderer->endFrame();
            device->endFrame();
        }
        device->waitIdle();
        Image img;
        img.width = width;
        img.height = height;
        img.rgba = device->readTexture(target);
        return img;
    }

    // Был ли в последнем завершённом кадре проход, в имени которого есть `part`.
    bool ranPass(std::string_view part) const {
        for (const PassTiming& p : renderer->stats().passes)
            if (p.name.find(part) != std::string::npos) return true;
        return false;
    }

    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<Renderer> renderer;
    std::unique_ptr<World> world;
    RenderSnapshot snapshot;
    rhi::TextureHandle target;
    ViewId view = 0;

private:
    std::string m_error;
    bool m_created = false;
    u32 m_materialCounter = 0;
};

} // namespace guide
