// Глава 19: материалы на GPU — .oxmat и MaterialAsset в коде через GpuResourceCache, металл/диэлектрик,
// emissive относительно экспозиции, AlphaTest, двусторонние, Transparent, Refractive с поглощением, замена
// материала на лету и проект с AssetManager + hot reload (docs/guide/19-materials.md).
// Нужен Vulkan; без него тесты пропускаются.
#include "../18-rendering-overview/guide_render_fixture.hpp"

#include <oxwald/assets/assets.hpp>
#include <oxwald/render/asset_provider.hpp>

#include <fstream>

using namespace ox;
using namespace ox::render;
using namespace guide;

namespace {

fs::path materialsDir() { return fs::path(OX_GUIDE_DIR) / "materials"; }

class GuideMaterialsGpu : public GuideRenderTest {
protected:
    // Материал из .oxmat-файла, зарегистрированный в кэше рендерера под своим UUID.
    Uuid fromFile(const std::string& file) {
        Result<assets::MaterialAsset> m = assets::loadMaterialFile(materialsDir() / file);
        EXPECT_TRUE(m) << file;
        const Uuid id = Uuid::fromName("Materials/" + file);
        renderer->resources().addMaterial(id, m ? *m : assets::MaterialAsset{});
        return id;
    }
    // Средний цвет квадрата (2r+1)² вокруг пикселя, 0..255.
    static glm::vec3 color(const Image& img, glm::ivec2 p, int r = 2) {
        glm::vec3 s(0.0f);
        for (int y = -r; y <= r; ++y)
            for (int x = -r; x <= r; ++x) s += glm::vec3(glm::vec4(img.at(u32(p.x + x), u32(p.y + y))));
        return s / f32((2 * r + 1) * (2 * r + 1));
    }
};

} // namespace

TEST_F(GuideMaterialsGpu, OxmatFilesRender) {
    const Uuid plastic = fromFile("red_plastic.oxmat");
    const Uuid neon = fromFile("neon_sign.oxmat");
    const Uuid gold = fromFile("brushed_gold.oxmat");
    mesh(Primitive::Sphere, plastic, {-1.2f, 0, 0});
    mesh(Primitive::Cube, neon, {0, 0, 0}, glm::vec3(0.8f));
    mesh(Primitive::Sphere, gold, {1.2f, 0, 0});
    sun({-0.3f, -0.8f, -0.5f}, 20000.0f);
    environment();
    const CameraParams cam = camera({0, 0.5f, 4.5f}, {0, 0, 0}, 12.0f);
    const Image img = render(cam);

    const glm::vec3 p = color(img, project(cam, {-1.2f, 0.25f, 0.45f}, 256, 256));
    const glm::vec3 n = color(img, project(cam, {0, 0, 0.4f}, 256, 256));
    const glm::vec3 g = color(img, project(cam, {1.2f, 0, 0}, 256, 256), 8); // весь диск сферы, не только блик
    EXPECT_GT(p.r, p.g + 60.0f) << "красный пластик";
    EXPECT_GT(n.r, 200.0f) << "неон светится сам";
    EXPECT_GT(n.b, n.g) << "розовый неон";
    EXPECT_GT(g.r, g.b + 20.0f) << "золото: цветное отражение металла";
}

TEST_F(GuideMaterialsGpu, EmissiveIsExposureRelative) {
    // Тёмная сцена без света: видно только излучение.
    assets::MaterialAsset glow;
    glow.baseColor = {0, 0, 0, 1};
    glow.emissive = {1.0f, 1.0f, 1.0f};
    glow.emissiveStrength = 0.5f; // GpuMaterial::emissive = emissive × emissiveStrength
    mesh(Primitive::Cube, addMaterial(glow), {0, 0, 0}, glm::vec3(2.0f));
    CVarScope sky("r.Sky", "false");
    const Image dim = render(camera({0, 0, 4}, {0, 0, 0}, 6.0f));     // EV100 6 — интерьер
    const Image bright = render(camera({0, 0, 4}, {0, 0, 0}, 15.0f)); // EV100 15 — солнечный день
    const f32 a = dim.luminance(128, 128), b = bright.luminance(128, 128);
    EXPECT_GT(a, 0.3f);
    EXPECT_NEAR(a, b, 0.02f) << "emissive 1 = белый при текущей экспозиции, от EV100 не зависит";
}

TEST_F(GuideMaterialsGpu, AlphaTestAndDoubleSided) {
    // Красная стена позади, перед ней зелёная плоскость (нормаль +Z после поворота).
    mesh(Primitive::Cube, material({0.9f, 0.05f, 0.05f, 1}), {0, 0, -1.5f}, {6.0f, 6.0f, 0.2f});
    assets::MaterialAsset leaf;
    leaf.blendMode = assets::BlendMode::AlphaTest;
    leaf.baseColor = {0.1f, 0.9f, 0.1f, 0.3f}; // альфа 0.3 (с текстурой — альфа текстуры × baseColor.a)
    leaf.alphaCutoff = 0.5f;                   // < cutoff — пиксель отброшен
    const Uuid cut = addMaterial(leaf);
    leaf.alphaCutoff = 0.2f;
    const Uuid kept = addMaterial(leaf);

    Entity quad = mesh(Primitive::Plane, cut, {0, 0, 0}, glm::vec3(2.0f));
    quad.setRotation(glm::angleAxis(glm::radians(90.0f), glm::vec3(1, 0, 0))); // лицом к камере (+Z)
    sun({-0.3f, -0.5f, -0.8f}, 20000.0f);
    environment();
    const CameraParams front = camera({0, 0, 4}, {0, 0, 0}, 12.0f);
    {
        const glm::u8vec4 c = render(front).at(128, 128);
        EXPECT_GT(c.r, c.g + 40) << "alpha < cutoff: видна красная стена";
    }
    quad.get<MeshRendererComponent>().materials = {kept};
    {
        const glm::u8vec4 c = render(front).at(128, 128);
        EXPECT_GT(c.g, c.r + 40) << "alpha ≥ cutoff: зелёная плоскость";
    }
    // Вид сзади: односторонняя плоскость отсекается (back-face culling), двусторонняя видна.
    quad.get<MeshRendererComponent>().materials = {material({0.1f, 0.9f, 0.1f, 1})};
    const CameraParams back = camera({0, 0, -1.2f}, {0, 0, 1}, 12.0f);
    auto green = [](glm::u8vec4 c) { return c.g > c.r + 40 && c.g > c.b + 20; };
    EXPECT_FALSE(green(render(back).at(128, 128))) << "односторонняя: сзади не видна (видно небо)";
    assets::MaterialAsset twoSided;
    twoSided.baseColor = {0.1f, 0.9f, 0.1f, 1};
    twoSided.doubleSided = true; // нормаль разворачивается к зрителю на обратной стороне
    quad.get<MeshRendererComponent>().materials = {addMaterial(twoSided)};
    EXPECT_TRUE(green(render(back).at(128, 128))) << "двусторонняя: видна и сзади";
}

TEST_F(GuideMaterialsGpu, TransparentBlendsWithBackground) {
    mesh(Primitive::Cube, material({0.9f, 0.05f, 0.05f, 1}), {0, 0, -1.5f}, {6.0f, 6.0f, 0.2f});
    Entity window = mesh(Primitive::Cube, fromFile("tinted_window.oxmat"), {0, 0, 0}, {2.0f, 2.0f, 0.02f});
    (void)window;
    sun({-0.3f, -0.5f, -0.8f}, 20000.0f);
    environment();
    const Image img = render(camera({0, 0, 4}, {0, 0, 0}, 12.0f), 256, 256, 4);
    EXPECT_TRUE(ranPass("Translucency.")); // OIT или Sorted — r.Translucency.Method
    const glm::u8vec4 c = img.at(128, 128), wall = img.at(10, 128);
    EXPECT_GT(c.b, wall.b + 20) << "синий оттенок стекла";
    EXPECT_GT(c.r, 40) << "красная стена просвечивает";
}

TEST_F(GuideMaterialsGpu, RefractiveGlassAbsorbs) {
    mesh(Primitive::Plane, material({0.8f, 0.8f, 0.8f, 1}, 0.0f, 0.8f), {0, 0, 0}, glm::vec3(10.0f));
    mesh(Primitive::Sphere, fromFile("green_glass.oxmat"), {0, 0.75f, 0}, glm::vec3(1.5f));
    sun(glm::normalize(glm::vec3(-0.4f, -0.8f, -0.45f)), 30000.0f);
    environment();
    const CameraParams cam = camera({0.0f, 2.4f, 3.2f}, {0, 0.5f, 0}, 13.5f);
    const Image img = render(cam, 256, 256, 4);
    EXPECT_TRUE(ranPass("Translucency.Refractive"));
    const glm::vec3 centre = color(img, project(cam, {0, 0.75f, 0}, 256, 256));
    EXPECT_GT(centre.g, centre.r + 10.0f) << "Beer–Lambert: зелёный оттенок по толщине";
}

TEST_F(GuideMaterialsGpu, ReplaceMaterialAtRuntime) {
    const Uuid id = Uuid::fromName("Materials/runtime_paint.oxmat");
    assets::MaterialAsset m;
    m.baseColor = {0.9f, 0.05f, 0.05f, 1};
    renderer->resources().addMaterial(id, m);
    mesh(Primitive::Cube, id, {0, 0, 0}, glm::vec3(2.0f));
    sun({-0.3f, -0.5f, -0.8f}, 20000.0f);
    environment();
    const CameraParams cam = camera({0, 0, 4}, {0, 0, 0}, 12.0f);
    EXPECT_GT(render(cam).at(128, 128).r, 100);
    m.baseColor = {0.05f, 0.05f, 0.9f, 1};
    renderer->resources().addMaterial(id, m); // тот же UUID: запись заменяется, меши не трогаем
    const glm::u8vec4 c = render(cam).at(128, 128);
    EXPECT_GT(c.b, c.r + 40);
}

TEST_F(GuideMaterialsGpu, ProjectMaterialsThroughAssetManagerWithHotReload) {
    // Проект: Assets/Materials/paint.oxmat. Так материалы попадают в рендер в игре и редакторе
    // (RuntimeRenderer делает setProvider + connectAssetHotReload сам).
    const fs::path project = fs::temp_directory_path() / ("oxwald_guide_matproj_" + Uuid::generate().toString());
    fs::create_directories(project / "Assets/Materials");
    const fs::path file = project / "Assets/Materials/paint.oxmat";
    fs::copy_file(materialsDir() / "red_plastic.oxmat", file);

    assets::AssetRegistry registry(project);
    registry.scan();
    {
        assets::AssetManager assets(registry);
        renderer->resources().setProvider(makeAssetManagerProvider(assets));
        ScopedConnection hotReload = connectAssetHotReload(assets, renderer->resources());

        const Uuid paint = *registry.uuidForPath("Materials/paint.oxmat");
        mesh(Primitive::Cube, paint, {0, 0, 0}, glm::vec3(2.0f));
        sun({-0.3f, -0.5f, -0.8f}, 20000.0f);
        environment();
        const CameraParams cam = camera({0, 0, 4}, {0, 0, 0}, 12.0f);
        const glm::u8vec4 red = render(cam).at(128, 128);
        EXPECT_GT(red.r, red.g + 60) << "материал загружен из проекта";

        // Художник поменял файл → менеджер перезагружает ассет → onReloaded → кэш рендерера обновляет материал.
        {
            std::ofstream out(file, std::ios::trunc);
            out << R"({"oxmat": 1, "baseColor": [0.05, 0.8, 0.1, 1.0], "roughness": 0.35})";
        }
        assets.reload(paint); // в редакторе это делает слежение за файлами (registry poll)
        assets.waitAll();     // + update(): подмена и onReloaded
        const glm::u8vec4 green = render(cam).at(128, 128);
        EXPECT_GT(green.g, green.r + 40) << "hot reload материала";

        device->waitIdle();
        renderer->resources().setProvider({}); // провайдер не должен пережить AssetManager
    }
    fs::remove_all(project);
}
