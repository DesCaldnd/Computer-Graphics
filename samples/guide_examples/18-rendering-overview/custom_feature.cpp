// Глава 18: своя render feature (compute SSAO из Depth + Normals → AO), её cvar'ы и переключатель
// r.Feature.<Name>, разрешение рендера (r.ScreenPercentage), отладочные режимы просмотра, hot reload шейдера
// фичи, статистика кадра (docs/guide/18-rendering-overview.md). Нужен Vulkan; без него тесты пропускаются.
#include "guide_render_fixture.hpp"

#include <oxwald/render/editor_viewport.hpp>

#include <chrono>
#include <fstream>
#include <thread>

using namespace ox;
using namespace ox::render;
using namespace guide;

namespace {

// --- фича -------------------------------------------------------------------------------------------------

// Cvar'ы фичи: обычные CVar'ы ядра (глава 04). Радиус зависит от группы качества Effects (Low…Ultra).
CVar<bool> cvGuideAo("r.GuideAO", true, "Guide: screen-space ambient occlusion");
CVar<float> cvGuideAoRadius("r.GuideAO.Radius", 0.5f, "Guide AO radius (m)", Scalability::Effects,
                            {0.3f, 0.5f, 0.5f, 0.8f});
CVar<float> cvGuideAoIntensity("r.GuideAO.Intensity", 1.5f, "Guide AO strength");

class GuideAoFeature final : public IRenderFeature {
public:
    std::string_view name() const override { return "GuideAO"; } // → переключатель r.Feature.GuideAO
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::Lighting); }
    // Группа "AO": в ней же встроенная AmbientOcclusion (GTAO/SSAO, priority 0) и RT AO. Работает одна
    // включённая фича группы с наибольшим priority — наша заменяет встроенную.
    std::string_view exclusiveGroup() const override { return "AO"; }
    i32 priority() const override { return 10; }
    std::vector<std::string_view> provides() const override { return {res::kAO}; }
    std::vector<std::string> cvarNames() const override { return {"r.GuideAO", "r.GuideAO.Radius", "r.GuideAO.Intensity"}; }
    bool isEnabled(const RenderSettings&, const rhi::DeviceCaps&) const override { return cvGuideAo; }

    bool initialize(FeatureInitContext& ctx) override {
        // Путь относительно корней шейдеров: DeviceDesc::shaderOptions.includeRoots, затем engine/shaders.
        // Пайплайн из файла перезагружается сам при правке шейдера (hot reload).
        m_pipeline = createComputePipeline(ctx.device, "guide.ao", "guide/simple_ao.comp");
        return bool(m_pipeline); // false — фича выключается навсегда
    }
    void shutdown(rhi::Device& device) override { device.destroy(m_pipeline); }

    void setup(FeatureContext& ctx) override {
        FrameResources& R = ctx.resources();
        const rhi::RGTexture depth = R.texture(res::kDepth);
        const rhi::RGTexture normals = R.texture(res::kNormals);
        const Extent2D e = ctx.renderExtent(); // разрешение рендера, не вывода
        lastExtent = e;

        rhi::TextureDesc td;
        td.format = formats::kAO; // R8_UNORM
        td.width = e.width;
        td.height = e.height;
        td.usage = rhi::TextureUsage::None; // граф сам выведет Storage | Sampled из объявлений
        td.name = "GuideAO";
        const rhi::RGTexture ao = ctx.graph().createTexture(td); // транзиентная: память переиспользуется

        const VkDeviceAddress view = ctx.viewAddress(), scene = ctx.sceneAddress();
        const f32 radius = cvGuideAoRadius, intensity = cvGuideAoIntensity;
        ctx.graph()
            .addPass("GuideAO", rhi::PassType::Compute)
            .read(depth, rhi::Access::SampledCompute)
            .read(normals, rhi::Access::SampledCompute)
            .overwrite(ao, rhi::Access::StorageWriteCompute)
            .execute([=, this](rhi::PassContext& p) {
                // Совпадает с OX_RENDER_PUSH(uint depth; uint normals; uint outAo; float radius; float intensity;)
                struct {
                    u64 view, scene;
                    u32 depth, normals, outAo;
                    f32 radius, intensity;
                } pc{view, scene, p.sampledIndex(depth), p.sampledIndex(normals), p.storageIndex(ao), radius, intensity};
                p.cmd.bindPipeline(m_pipeline);
                p.cmd.pushConstants(pc);
                p.cmd.dispatchThreads(e.width, e.height);
            });
        R.setTexture(res::kAO, ao); // ForwardOpaque прочитает AO и умножит на него непрямой свет
    }

    Extent2D lastExtent;

private:
    rhi::PipelineHandle m_pipeline;
};

// Регистрация «для всех рендереров»: фабрика, вызываемая из явной registerXxx() вашего модуля.
void registerGuideAo() {
    registerFeatureFactory("GuideAO", [] { return std::make_unique<GuideAoFeature>(); });
}

// --- сцена ---------------------------------------------------------------------------------------------------

class GuideRenderer : public GuideRenderTest {
protected:
    // Куб на плоскости, только окружающий свет (AO влияет лишь на непрямое освещение).
    void cubeOnPlane() {
        const Uuid grey = material({0.8f, 0.8f, 0.8f, 1.0f}, 0.0f, 0.9f);
        mesh(Primitive::Plane, grey, {0, 0, 0}, glm::vec3(20.0f));
        mesh(Primitive::Cube, grey, {0, 0.5f, 0});
        environment(1.0f, 1.0f);
    }
    void SetUp() override {
        GuideRenderTest::SetUp();
        // Встроенный AO выключен, чтобы сравнивать «без AO» и «с нашим AO».
        if (device) m_noBuiltinAo = std::make_unique<CVarScope>("r.AO.Method", "0");
    }
    void TearDown() override {
        m_noBuiltinAo.reset();
        GuideRenderTest::TearDown();
    }
    std::unique_ptr<CVarScope> m_noBuiltinAo;
    const CameraParams cam = camera({0.0f, 2.0f, 3.5f}, {0.0f, 0.3f, 0.0f}, 11.0f);
};

} // namespace

TEST_F(GuideRenderer, FrameRunsCorePasses) {
    cubeOnPlane();
    sun({-0.3f, -0.8f, -0.5f});
    const Image img = render(cam, 256, 256, 3);
    const RenderStats& s = renderer->stats(); // последний завершённый кадр
    EXPECT_TRUE(ranPass("DepthPrepass"));
    EXPECT_TRUE(ranPass("LightCulling"));
    EXPECT_TRUE(ranPass("ForwardOpaque"));
    EXPECT_TRUE(ranPass("Tonemap"));
    EXPECT_GE(s.instances, 2u);
    EXPECT_GT(s.drawCalls, 0u);
    EXPECT_GT(s.renderGraphPasses, 5u);
    EXPECT_GT(img.luminance(128, 160), 0.05f) << "куб и плоскость освещены";
}

TEST_F(GuideRenderer, CustomAoFeatureDarkensCreases) {
    cubeOnPlane();
    const glm::ivec2 crease = project(cam, {0.0f, 0.03f, 0.56f}, 256, 256);  // у основания куба
    const glm::ivec2 open = project(cam, {0.9f, 0.0f, 1.2f}, 256, 256);      // открытый пол
    ASSERT_TRUE(open.x > 3 && open.x < 252 && open.y > 3 && open.y < 252);
    render(cam); // прогрев: IBL окружения строится в первых кадрах
    const Image base = render(cam);

    auto& ao = renderer->features().emplace<GuideAoFeature>(); // только этот Renderer
    const Image withAo = render(cam);
    EXPECT_TRUE(ranPass("GuideAO"));
    EXPECT_LT(withAo.meanLuminance(crease, 3), base.meanLuminance(crease, 3) * 0.95f) << "щель у основания темнее";
    EXPECT_NEAR(withAo.meanLuminance(open, 3), base.meanLuminance(open, 3), 0.03f) << "открытый пол не затеняется";
    EXPECT_EQ(ao.lastExtent, (Extent2D{256, 256}));

    {   // Переключатель r.Feature.<Name> создаётся автоматически; граф перестроится на следующем кадре.
        CVarScope off("r.Feature.GuideAO", "false");
        const Image disabled = render(cam);
        EXPECT_FALSE(ranPass("GuideAO"));
        EXPECT_NEAR(disabled.meanLuminance(crease, 3), base.meanLuminance(crease, 3), 0.02f);
    }
    {   // Внутреннее разрешение: 50 % от вывода по каждой оси, апскейл до 256² — встроенный bilinear Resample.
        CVarScope half("r.ScreenPercentage", "50");
        render(cam);
        EXPECT_EQ(ao.lastExtent, (Extent2D{128, 128}));
    }
}

TEST_F(GuideRenderer, DebugViews) {
    mesh(Primitive::Plane, material({0.9f, 0.1f, 0.1f, 1.0f}, 0.0f, 0.5f), {0, 0, 0}, glm::vec3(20.0f));
    sun({-0.3f, -0.8f, -0.5f});
    const CameraParams top = camera({0.0f, 5.0f, 0.01f}, {0, 0, 0}, 12.0f);
    {
        CVarScope dv("r.DebugView", "Albedo"); // базовый цвет без освещения и тонмаппинга
        const glm::u8vec4 c = render(top).at(128, 128);
        EXPECT_GT(int(c.r), int(c.g) + 100);
    }
    {
        CVarScope dv("r.DebugView", "Normals"); // n * 0.5 + 0.5: нормаль (0, 1, 0) → зелёный
        const glm::u8vec4 c = render(top).at(128, 128);
        EXPECT_GT(c.g, 240);
        EXPECT_NEAR(c.r, 128, 40);
    }
    // Тот же режим без cvar'а — только для одного вида: ViewRenderRequest::settingsOverride.
    EXPECT_STREQ(debugViewName(DebugView::LightComplexity), "LightComplexity");
}

TEST_F(GuideRenderer, EditorViewportRendersToImage) {
    renderer.reset(); // у редакторского пути свой Renderer
    EditorViewportRenderer evr(*device);
    assets::MaterialAsset red;
    red.baseColor = {0.9f, 0.1f, 0.1f, 1.0f};
    const Uuid redId = Uuid::generate();
    evr.renderer().resources().addMaterial(redId, red);
    Entity cube = world->create("Cube");
    auto& mr = cube.add<MeshRendererComponent>();
    mr.mesh = primitiveUuid(Primitive::Cube);
    mr.materials = {redId};
    sun({-0.3f, -0.8f, -0.5f});
    world->updateTransforms();
    world->snapshotPreviousTransforms();

    EditorViewportFrame f;
    f.world = world.get();
    f.camera = camera({0, 0, 4}, {0, 0, 0}, 12.0f);
    f.grid = false;
    const std::vector<u8> px = evr.renderToImage(f, 64, 64); // миниатюры, тесты
    ASSERT_EQ(px.size(), 64u * 64u * 4u);
    const usize centre = (32u * 64u + 32u) * 4u;
    EXPECT_GT(px[centre], px[centre + 1] + 40) << "красный куб в центре";
    EXPECT_EQ(evr.pick(f, {64, 64}, {32, 32}), std::optional<Uuid>(cube.uuid()));
}

TEST_F(GuideRenderer, HotReloadOfFeatureShader) {
    // Копия шейдеров игры во временном каталоге: её и будем править.
    const fs::path root = fs::temp_directory_path() /
                          std::format("oxwald_guide_hot_{}", std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(root);
    fs::copy(fs::path(OX_GUIDE_DIR) / "shaders", root, fs::copy_options::recursive);
    destroyDevice();
    ASSERT_TRUE(createDevice({root}));

    cubeOnPlane();
    renderer->features().emplace<GuideAoFeature>();
    const glm::ivec2 open = project(cam, {0.9f, 0.0f, 1.2f}, 256, 256);
    const Image before = render(cam);

    // Правка: AO = 0 везде → непрямой свет исчезает.
    const fs::path file = root / "guide/simple_ao.comp";
    std::string src;
    {
        std::ifstream in(file);
        src.assign(std::istreambuf_iterator<char>(in), {});
    }
    const std::string line = "    OX_IMAGE_STORE_2D(r8, pc.outAo, p, vec4(ao));";
    ASSERT_NE(src.find(line), std::string::npos);
    src.replace(src.find(line), line.size(), "    OX_IMAGE_STORE_2D(r8, pc.outAo, p, vec4(0.0));");
    std::this_thread::sleep_for(std::chrono::milliseconds(1100)); // разрешение mtime
    {
        std::ofstream out(file, std::ios::trunc);
        out << src;
    }
    // В игре это делает Device::beginFrame раз в hotReloadPollMs; здесь — явно и сразу.
    EXPECT_GE(device->reloadChangedShaders(true), 1u);
    const Image after = render(cam);
    EXPECT_LT(after.meanLuminance(open, 3), before.meanLuminance(open, 3) * 0.5f);
    destroyDevice();
    fs::remove_all(root);
}

// Последним: фабрика процесс-глобальна и попала бы во все Renderer последующих тестов этого процесса.
TEST_F(GuideRenderer, FeatureFactoryAppliesToNewRenderers) {
    registerGuideAo();
    renderer.reset();
    renderer = Renderer::create(*device); // фабрики инстанцируются при создании
    ASSERT_NE(renderer->features().find("GuideAO"), nullptr);
    cubeOnPlane();
    render(cam);
    EXPECT_TRUE(ranPass("GuideAO"));
    EXPECT_TRUE(renderer->features().remove("GuideAO", device.get())); // фичу можно и удалить на ходу
}

