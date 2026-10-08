#include "render_fixture.hpp"

#include <oxwald/render/features/postprocess/postprocess.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_write.h>

#include <cstdlib>

namespace ox::render::test {

namespace fs = std::filesystem;

bool writePng(const fs::path& path, const Image& img) {
    fs::create_directories(path.parent_path());
    return stbi_write_png(path.string().c_str(), int(img.width), int(img.height), 4, img.rgba.data(),
                          int(img.width * 4)) != 0;
}

bool readPng(const fs::path& path, Image& img) {
    int w = 0, h = 0, n = 0;
    stbi_uc* data = stbi_load(path.string().c_str(), &w, &h, &n, 4);
    if (!data) return false;
    img.width = u32(w);
    img.height = u32(h);
    img.rgba.assign(data, data + usize(w) * h * 4);
    stbi_image_free(data);
    return true;
}

GoldenResult compareGolden(const std::string& name, const Image& img, f64 maxMean, f64 maxBadFraction, u32 threshold) {
    GoldenResult r;
    const fs::path out = fs::temp_directory_path() / "oxwald_render_out" / (name + ".png");
    writePng(out, img);
    const fs::path golden = fs::path(OX_TEST_DATA_DIR) / "golden" / (name + ".png");
    const char* update = std::getenv("OX_UPDATE_GOLDEN");
    if (update && std::string(update) == "1") {
        r.written = writePng(golden, img);
        r.matched = r.written;
        r.message = "golden written: " + golden.string();
        return r;
    }
    Image ref;
    if (!readPng(golden, ref)) {
        r.message = "missing golden " + golden.string() + " (run with OX_UPDATE_GOLDEN=1); actual image: " + out.string();
        return r;
    }
    if (ref.width != img.width || ref.height != img.height) {
        r.message = std::format("size mismatch: golden {}x{}, actual {}x{}", ref.width, ref.height, img.width, img.height);
        return r;
    }
    u64 sum = 0, bad = 0;
    for (usize p = 0; p < usize(img.width) * img.height; ++p) {
        bool isBad = false;
        for (u32 ch = 0; ch < 3; ++ch) {
            const u32 d = u32(std::abs(int(img.rgba[p * 4 + ch]) - int(ref.rgba[p * 4 + ch])));
            sum += d;
            isBad |= d > threshold;
        }
        bad += isBad ? 1 : 0;
    }
    const f64 pixels = f64(img.width) * img.height;
    r.meanError = f64(sum) / (pixels * 3.0);
    r.badFraction = f64(bad) / pixels;
    r.matched = r.meanError <= maxMean && r.badFraction <= maxBadFraction;
    r.message = std::format("{}: mean error {:.3f} (max {}), bad pixels {:.3f}% (max {}%); actual: {}", name, r.meanError,
                            maxMean, r.badFraction * 100.0, maxBadFraction * 100.0, out.string());
    return r;
}

CVarScope::CVarScope(std::string name, std::string value) : m_name(std::move(name)) {
    ICVar* c = CVarRegistry::instance().find(m_name);
    if (c) m_previous = c->toString();
    CVarRegistry::instance().set(m_name, value, CVarSource::Code);
}

CVarScope::~CVarScope() {
    if (!m_previous.empty()) CVarRegistry::instance().set(m_name, m_previous, CVarSource::Code);
}

void RenderTest::SetUp() {
    registerSceneTypes();
    rhi::Device::resetValidationCounters();
    rhi::DeviceDesc desc;
    desc.appName = "ox_render_gpu_tests";
    desc.validation = true;
    desc.shaderOptions.cacheDirectory = fs::temp_directory_path() / "oxwald_render_test_shader_cache";
    appendUpscalerVulkanExtensions(desc); // NGX (DLSS) extensions where DLSS can run, like the runtime renderer
    std::string error;
    device = rhi::Device::create(desc, &error);
    if (!device) GTEST_SKIP() << "no Vulkan device: " << error;
    renderer = Renderer::create(*device);
    world = std::make_unique<World>();
}

void RenderTest::TearDown() {
    if (!device) return;
    device->waitIdle();
    renderer.reset();
    if (target) device->destroy(target);
    world.reset();
    device.reset();
    EXPECT_EQ(rhi::Device::validationErrorCount(), 0u) << "Vulkan validation reported errors (see log)";
}

Uuid RenderTest::material(glm::vec4 baseColor, f32 metallic, f32 roughness, glm::vec3 emissive, assets::BlendMode blend) {
    assets::MaterialAsset m;
    m.baseColor = baseColor;
    m.metallic = metallic;
    m.roughness = roughness;
    m.emissive = emissive;
    m.blendMode = blend;
    const Uuid id = Uuid::fromName(std::format("test.material.{}", materialCounter++));
    renderer->resources().addMaterial(id, m);
    return id;
}

Entity RenderTest::mesh(Primitive p, const Uuid& mat, glm::vec3 position, glm::vec3 scale, glm::quat rotation) {
    Entity e = world->create(primitiveName(p));
    e.setPosition(position);
    e.setScale(scale);
    e.setRotation(rotation);
    auto& mr = e.add<MeshRendererComponent>();
    mr.mesh = primitiveUuid(p);
    mr.materials = {mat};
    return e;
}

Entity RenderTest::sun(glm::vec3 direction, f32 lux, glm::vec3 color, bool shadows) {
    Entity e = world->create("Sun");
    e.setRotation(lookRotation(glm::normalize(direction), std::abs(direction.y) > 0.99f ? glm::vec3(1, 0, 0) : kWorldUp));
    auto& l = e.add<LightComponent>();
    l.type = LightType::Directional;
    l.intensity = lux;
    l.color = color;
    l.castShadows = shadows;
    l.sourceRadius = 0.5f;
    return e;
}

Entity RenderTest::pointLight(glm::vec3 position, f32 lumens, f32 range, glm::vec3 color, bool shadows) {
    Entity e = world->create("Point");
    e.setPosition(position);
    auto& l = e.add<LightComponent>();
    l.type = LightType::Point;
    l.intensity = lumens;
    l.range = range;
    l.color = color;
    l.castShadows = shadows;
    return e;
}

Entity RenderTest::spotLight(glm::vec3 position, glm::vec3 direction, f32 lumens, f32 range, f32 inner, f32 outer,
                             bool shadows, glm::vec3 color) {
    Entity e = world->create("Spot");
    e.setPosition(position);
    e.setRotation(lookRotation(glm::normalize(direction), std::abs(direction.y) > 0.99f ? glm::vec3(1, 0, 0) : kWorldUp));
    auto& l = e.add<LightComponent>();
    l.type = LightType::Spot;
    l.intensity = lumens;
    l.range = range;
    l.innerConeAngle = inner;
    l.outerConeAngle = outer;
    l.color = color;
    l.castShadows = shadows;
    return e;
}

Entity RenderTest::environment(f32 skyIntensity, f32 ambient) {
    Entity e = world->create("Environment");
    auto& env = e.add<EnvironmentComponent>();
    env.skyIntensity = skyIntensity;
    env.ambientIntensity = ambient;
    return e;
}

CameraParams RenderTest::camera(glm::vec3 eye, glm::vec3 targetPos, f32 ev100, f32 fov, f32 farPlane) {
    CameraParams c = CameraParams::lookAt(eye, targetPos, fov, 0.1f, farPlane);
    c.ev100 = ev100;
    return c;
}

void RenderTest::extractSnapshot() {
    world->updateTransforms();
    world->snapshotPreviousTransforms();
    debugDraw.flush(0.0f);
    const std::vector<u32> selection = snapshot.selection;
    extract(*world, snapshot, {.debugDraw = &debugDraw});
    snapshot.selection = selection;
}

void RenderTest::ensureTarget(u32 width, u32 height, VkFormat format) {
    if (target) {
        const rhi::TextureDesc& d = device->desc(target);
        if (d.width == width && d.height == height && d.format == format) return;
        device->destroy(target);
    }
    rhi::TextureDesc d;
    d.name = "test.target";
    d.format = format;
    d.width = width;
    d.height = height;
    d.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferSrc;
    target = device->createTexture(d);
}

void RenderTest::ensureView(bool editor) {
    if (view && viewIsEditor == editor) return;
    if (view) renderer->destroyView(view);
    ViewDesc vd;
    vd.name = editor ? "EditorView" : "TestView";
    vd.flags.editor = editor;
    view = renderer->createView(vd);
    viewIsEditor = editor;
}

Image RenderTest::render(const CameraParams& cam, const Options& o) {
    ensureTarget(o.width, o.height, o.format);
    ensureView(o.editor);
    extractSnapshot();
    renderer->resources().flush();
    for (u32 f = 0; f < o.frames; ++f) {
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
    img.width = o.width;
    img.height = o.height;
    img.rgba = device->readTexture(target);
    for (usize i = 3; i < img.rgba.size(); i += 4) img.rgba[i] = 255;
    return img;
}

std::string asciiArt(const Image& img, u32 columns) {
    static const char* ramp = " .:-=+*#%@";
    std::string s;
    const u32 rows = columns * img.height / img.width / 2;
    for (u32 r = 0; r < rows; ++r) {
        for (u32 c = 0; c < columns; ++c) {
            const f32 l = img.luminance(c * img.width / columns, r * img.height / rows);
            s += ramp[std::min(9, int(l * 9.99f))];
        }
        s += '\n';
    }
    return s;
}

} // namespace ox::render::test
