#pragma once

// Shared fixture for render GPU tests: headless device (validation on, skips without Vulkan), a Renderer, test
// scenes built through the real ECS + extract path, offscreen rendering and golden image comparison.
//
// Goldens live in engine/render/tests/data/golden/<name>.png. With OX_UPDATE_GOLDEN=1 the test (re)writes them;
// every run also writes the actual image to <temp>/oxwald_render_out/<name>.png for inspection.

#include <oxwald/core/cvar.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/render/render.hpp>
#include <oxwald/rhi/device.hpp>
#include <oxwald/scene/scene.hpp>
#include <oxwald/scene/world.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ox::render::test {

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
};

bool writePng(const std::filesystem::path& path, const Image& img);
bool readPng(const std::filesystem::path& path, Image& img);

struct GoldenResult {
    bool matched = false;
    bool written = false;
    f64 meanError = 0.0;   // mean absolute error per channel (0..255)
    f64 badFraction = 0.0; // fraction of pixels with any channel off by more than the threshold
    std::string message;
};

// Default tolerance: mean error < 2, at most 1% of pixels differing by more than 24/255 (GPU/driver noise, dither).
GoldenResult compareGolden(const std::string& name, const Image& img, f64 maxMean = 2.0, f64 maxBadFraction = 0.01,
                           u32 threshold = 24);

// Scoped cvar override: restores the previous value on destruction.
class CVarScope {
public:
    CVarScope(std::string name, std::string value);
    ~CVarScope();

private:
    std::string m_name, m_previous;
};

class RenderTest : public ::testing::Test {
protected:
    void SetUp() override;
    void TearDown() override;

    // --- scene building ---
    Uuid material(glm::vec4 baseColor, f32 metallic, f32 roughness, glm::vec3 emissive = glm::vec3(0.0f),
                  assets::BlendMode blend = assets::BlendMode::Opaque);
    Entity mesh(Primitive p, const Uuid& material, glm::vec3 position, glm::vec3 scale = glm::vec3(1.0f),
                glm::quat rotation = glm::quat(1, 0, 0, 0));
    Entity sun(glm::vec3 direction, f32 lux = 20000.0f, glm::vec3 color = glm::vec3(1.0f), bool shadows = true);
    Entity pointLight(glm::vec3 position, f32 lumens, f32 range, glm::vec3 color = glm::vec3(1.0f), bool shadows = false);
    Entity spotLight(glm::vec3 position, glm::vec3 direction, f32 lumens, f32 range, f32 inner, f32 outer,
                     bool shadows = true, glm::vec3 color = glm::vec3(1.0f));
    Entity environment(f32 skyIntensity = 1.0f, f32 ambient = 1.0f);

    // --- rendering ---
    struct Options {
        u32 width = 256, height = 256;
        u32 frames = 2;
        bool editor = false;
        VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    };
    // Extracts the world, renders `frames` frames from `camera` and reads back the last one.
    Image render(const CameraParams& camera, const Options& options);
    Image render(const CameraParams& camera) { return render(camera, Options{}); }
    void extractSnapshot();
    void ensureTarget(u32 width, u32 height, VkFormat format);
    void ensureView(bool editor);

    static CameraParams camera(glm::vec3 eye, glm::vec3 target, f32 ev100, f32 fov = 50.0f, f32 farPlane = 200.0f);

    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<Renderer> renderer;
    std::unique_ptr<World> world;
    RenderSnapshot snapshot;
    DebugDraw debugDraw;
    rhi::TextureHandle target;
    ViewId view = 0;
    bool viewIsEditor = false;
    u32 materialCounter = 0;
};

// Draws a slice of the image as ASCII luminance (debugging aid in failure messages).
std::string asciiArt(const Image& img, u32 columns = 64);

} // namespace ox::render::test
