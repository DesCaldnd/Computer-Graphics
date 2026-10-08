#pragma once

// Небольшая обвязка для GPU-примеров глав 20–22 (освещение, отражения, объёмные эффекты): headless Vulkan-устройство,
// Renderer, сцена в настоящем ECS-мире и рендер в offscreen-текстуру с чтением пикселей обратно.
// Это упрощённая версия engine/render/tests/gpu/render_fixture.* (тот фикстур не экспортируется из движка).
// Без Vulkan-устройства тесты пропускаются (GTEST_SKIP); любая ошибка validation layer валит тест.

#include <oxwald/assets/material.hpp>
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
#include <string_view>
#include <vector>

namespace guide {

using namespace ox;
using namespace ox::render;

// Временно меняет cvar (как из кода) и возвращает прежнее значение при выходе из области видимости.
class ScopedCVar {
public:
    ScopedCVar(std::string name, const std::string& value) : m_name(std::move(name)) {
        if (ICVar* c = CVarRegistry::instance().find(m_name)) m_previous = c->toString();
        CVarRegistry::instance().set(m_name, value, CVarSource::Code);
    }
    ~ScopedCVar() {
        if (!m_previous.empty()) CVarRegistry::instance().set(m_name, m_previous, CVarSource::Code);
    }
    ScopedCVar(const ScopedCVar&) = delete;
    ScopedCVar& operator=(const ScopedCVar&) = delete;

private:
    std::string m_name, m_previous;
};

struct Image {
    u32 width = 0, height = 0;
    std::vector<u8> rgba; // RGBA8, sRGB-кодированные значения после тонмаппинга

    [[nodiscard]] f32 luminance(u32 x, u32 y) const {
        const usize i = (usize(y) * width + x) * 4;
        return (0.2126f * rgba[i] + 0.7152f * rgba[i + 1] + 0.0722f * rgba[i + 2]) / 255.0f;
    }
    [[nodiscard]] f32 meanLuminance(u32 x0, u32 y0, u32 x1, u32 y1) const {
        f64 sum = 0.0;
        for (u32 y = y0; y < y1; ++y)
            for (u32 x = x0; x < x1; ++x) sum += luminance(x, y);
        return f32(sum / f64((x1 - x0) * (y1 - y0)));
    }
    [[nodiscard]] f32 meanLuminance() const { return meanLuminance(0, 0, width, height); }
};

// Средняя разница каналов RGB двух изображений (0..255).
inline f64 meanDifference(const Image& a, const Image& b) {
    u64 sum = 0;
    for (usize i = 0; i < a.rgba.size(); ++i) {
        if (i % 4 != 3) sum += u64(std::abs(int(a.rgba[i]) - int(b.rgba[i])));
    }
    return f64(sum) / f64(a.rgba.size() / 4 * 3);
}

class GuideRenderTest : public ::testing::Test {
protected:
    void SetUp() override {
        registerSceneTypes();
        rhi::Device::resetValidationCounters();
        rhi::DeviceDesc desc;
        desc.appName = "guide_render";
        desc.validation = true;
        desc.shaderOptions.cacheDirectory = std::filesystem::temp_directory_path() / "oxwald_guide_shader_cache";
        std::string error;
        device = rhi::Device::create(desc, &error);
        if (!device) GTEST_SKIP() << "нет Vulkan-устройства: " << error;
        renderer = Renderer::create(*device); // встроенные фичи: тени, IBL, небо, отражения, объёмный туман, ...
        world = std::make_unique<World>();
    }
    void TearDown() override {
        if (!device) return;
        device->waitIdle();
        renderer.reset();
        if (target) device->destroy(target);
        world.reset();
        device.reset();
        EXPECT_EQ(rhi::Device::validationErrorCount(), 0u) << "validation layer сообщил об ошибках (см. лог)";
    }

    // Материал без файла: кладётся прямо в кэш ресурсов рендера под выдуманным Uuid.
    Uuid material(glm::vec4 baseColor, f32 metallic, f32 roughness, glm::vec3 emissive = glm::vec3(0.0f)) {
        assets::MaterialAsset m;
        m.baseColor = baseColor;
        m.metallic = metallic;
        m.roughness = roughness;
        m.emissive = emissive;
        const Uuid id = Uuid::fromName(std::format("guide.material.{}", m_materialCounter++));
        renderer->resources().addMaterial(id, m);
        return id;
    }

    // Процедурный примитив (куб, сфера, плоскость, ...) на сцене.
    Entity mesh(Primitive p, const Uuid& mat, glm::vec3 position, glm::vec3 scale = glm::vec3(1.0f)) {
        Entity e = world->create(primitiveName(p));
        e.setPosition(position);
        e.setScale(scale);
        auto& mr = e.add<MeshRendererComponent>();
        mr.mesh = primitiveUuid(p);
        mr.materials = {mat};
        return e;
    }

    // Рендерит `frames` кадров (временным эффектам нужно несколько) и читает последний.
    Image render(const CameraParams& camera, u32 width = 128, u32 height = 128, u32 frames = 2) {
        ensureTarget(width, height);
        if (!view) view = renderer->createView({.name = "GuideView"});
        world->updateTransforms();
        world->snapshotPreviousTransforms();
        extract(*world, snapshot);
        renderer->resources().flush();
        for (u32 f = 0; f < frames; ++f) {
            device->beginFrame();
            renderer->beginFrame(snapshot);
            ViewRenderRequest req;
            req.view = view;
            req.camera = camera;
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

    // Был ли в последнем завершённом кадре проход, имя которого содержит `part` (имена: "<view>/<pass>").
    [[nodiscard]] bool ranPass(std::string_view part) const {
        for (const PassTiming& p : renderer->stats().passes) {
            if (p.name.find(part) != std::string::npos) return true;
        }
        return false;
    }

    static CameraParams camera(glm::vec3 eye, glm::vec3 at, f32 ev100, f32 fovDegrees = 50.0f, f32 farPlane = 200.0f) {
        CameraParams c = CameraParams::lookAt(eye, at, fovDegrees, 0.1f, farPlane);
        c.ev100 = ev100;
        return c;
    }

    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<Renderer> renderer;
    std::unique_ptr<World> world;
    RenderSnapshot snapshot;
    rhi::TextureHandle target;
    ViewId view = 0;

private:
    void ensureTarget(u32 width, u32 height) {
        if (target) {
            const rhi::TextureDesc& d = device->desc(target);
            if (d.width == width && d.height == height) return;
            device->destroy(target);
        }
        rhi::TextureDesc d;
        d.name = "guide.target";
        d.format = VK_FORMAT_R8G8B8A8_UNORM;
        d.width = width;
        d.height = height;
        d.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferSrc;
        target = device->createTexture(d);
    }
    u32 m_materialCounter = 0;
};

} // namespace guide
