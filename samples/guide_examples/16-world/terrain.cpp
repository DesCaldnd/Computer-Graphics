// Guide chapter 16 «Открытый мир»: heightfield, procedural generation, erosion, brushes, splat maps.
#include <oxwald/world/splat_map.hpp>
#include <oxwald/world/terrain_brush.hpp>
#include <oxwald/world/terrain_gen.hpp>

#include <gtest/gtest.h>

#include <glm/trigonometric.hpp>

#include <filesystem>

using namespace ox;
using namespace ox::world;

namespace {

// Ландшафт 512 × 512 м с шагом 2 м: 257 × 257 отсчётов.
Heightfield makeTerrain() {
    HeightfieldDesc d;
    d.resolution = 257;          // отсчётов на сторону → 256 квадов
    d.worldSize = 512.f;         // метров по X и Z
    d.heightScale = 120.f;       // высота = heightOffset + normalized * heightScale
    d.origin = {-256.f, -256.f}; // мировые XZ отсчёта (0, 0)
    d.format = HeightFormat::Float32;
    Heightfield hf(d);

    TerrainNoiseSettings ns;
    ns.fractal.basis = NoiseBasis::Simplex;
    ns.fractal.type = FractalType::Ridged; // хребты
    ns.fractal.seed = 7;
    ns.fractal.frequency = 1.f / 300.f;    // циклов на метр (первая октава)
    ns.fractal.octaves = 5;
    ns.fractal.warpStrength = 40.f;        // domain warp — «закрученный» рельеф
    generateNoise(hf, ns);                 // шум в мировых координатах: соседние тайлы стыкуются
    return hf;
}

} // namespace

TEST(GuideWorldTerrain, GenerateErodeAndSample) {
    Heightfield hf = makeTerrain();
    ASSERT_TRUE(hf.valid());
    EXPECT_FLOAT_EQ(hf.spacing(), 2.f);

    const HeightStats before = computeStats(hf);
    erodeHydraulic(hf, {.droplets = 20000, .seed = 1});           // капли воды размывают склоны
    erodeThermal(hf, {.iterations = 20, .talusAngleDeg = 38.f}); // осыпание слишком крутых склонов
    const HeightStats after = computeStats(hf);
    EXPECT_LT(after.maxSlopeDeg, before.maxSlopeDeg);
    EXPECT_NEAR(after.sum / before.sum, 1.0, 0.02) << "эрозия переносит материал, а не уничтожает";

    // Мировые запросы: билинейная высота, нормаль, уклон.
    const glm::vec2 p{10.f, -20.f};
    const f32 h = hf.sampleHeight(p);
    EXPECT_GE(h, hf.minHeight());
    EXPECT_LE(h, hf.maxHeight());
    EXPECT_GT(hf.sampleNormal(p).y, 0.f);
    EXPECT_LT(hf.sampleSlope(p), glm::radians(90.f));

    // Отсчёты хранятся нормализованными: можно сменить масштаб высот без пересчёта данных.
    hf.setScale(240.f, -10.f);
    EXPECT_NEAR(hf.sampleHeight(p), -10.f + (h / 120.f) * 240.f, 1e-3f);
}

TEST(GuideWorldTerrain, BrushesReturnDirtyRects) {
    Heightfield hf = makeTerrain();
    const f32 before = hf.sampleHeight({0.f, 0.f});

    // Кисть «поднять»: радиус 15 м, 4 м/с; dt делает мазок независимым от FPS.
    BrushSettings raise{.op = BrushOp::Raise, .radius = 15.f, .strength = 4.f, .falloff = BrushFalloff::Smooth};
    const IRect dirty = applyBrush(hf, {0.f, 0.f}, raise, /*dt*/ 0.5f);
    EXPECT_NEAR(hf.sampleHeight({0.f, 0.f}) - before, 2.f, 0.05f);
    // Прямоугольник изменённых отсчётов: его заливают в GPU-текстуру и по нему пересобирают коллайдеры.
    ASSERT_FALSE(dirty.empty());
    EXPECT_LE(dirty.width(), 17);
    const std::vector<u16> upload = hf.extractR16(dirty); // данные для частичного обновления R16_UNORM
    EXPECT_EQ(upload.size(), usize(dirty.width()) * usize(dirty.height()));

    // Выровнять площадку под здание и прорезать дыру (вход в пещеру).
    applyBrush(hf, {60.f, 60.f}, {.op = BrushOp::Flatten, .radius = 10.f, .strength = 1.f, .hardness = 0.5f, .targetHeight = 30.f});
    EXPECT_NEAR(hf.sampleHeight({60.f, 60.f}), 30.f, 0.5f);
    applyBrush(hf, {-60.f, 0.f}, {.op = BrushOp::SetHole, .radius = 3.f});
    EXPECT_TRUE(hf.hasHoles());
}

TEST(GuideWorldTerrain, AutoPaintSplatMap) {
    const Heightfield hf = makeTerrain();
    // 4 слоя материалов на той же площади: 0 = трава (база), 1 = скалы, 2 = снег, 3 = дорога.
    SplatMap splat(129, 4, hf.desc().origin, hf.desc().worldSize);
    splat.fill(0);
    const SplatRule rules[] = {
        {.layer = 1, .minSlopeDeg = 35.f, .maxSlopeDeg = 90.f},                     // скалы на крутых склонах
        {.layer = 2, .minHeight = 90.f, .heightBlend = 10.f, .maxSlopeDeg = 30.f}, // снег на вершинах
    };
    autoPaint(splat, hf, rules); // правила накладываются по порядку, веса нормализуются к 255

    // Ручная докраска кистью — та же модель спада, что у кисти высот.
    paintSplat(splat, {0.f, 0.f}, 3, {.radius = 8.f, .strength = 1.f, .falloff = BrushFalloff::Constant});
    EXPECT_EQ(splat.dominantLayer({0.f, 0.f}), 3u);

    const auto w = splat.weights(64, 64);
    f32 sum = 0.f;
    for (f32 v : w) {
        sum += v;
    }
    EXPECT_NEAR(sum, 1.f, 0.02f);
    // Для GPU: две RGBA8-текстуры (слои 0–3 и 4–7).
    EXPECT_EQ(splat.packRgba8(0, splat.fullRect()).size(), 129u * 129u * 4u);
}

TEST(GuideWorldTerrain, ExportAndImport16BitPng) {
    const Heightfield hf = makeTerrain();
    const auto path = std::filesystem::temp_directory_path() / "ox_guide_terrain.png";
    ASSERT_TRUE(hf.savePng16(path));

    std::string error;
    const std::optional<Heightfield> loaded =
        Heightfield::load(path, {.worldSize = 512.f, .heightScale = 120.f, .origin = {-256.f, -256.f}}, &error);
    ASSERT_TRUE(loaded) << error;
    EXPECT_EQ(loaded->resolution(), 257u);
    EXPECT_NEAR(loaded->sampleHeight({10.f, 10.f}), hf.sampleHeight({10.f, 10.f}), 0.01f); // 16 бит ≈ 2 мм на 120 м
    std::filesystem::remove(path);
}
