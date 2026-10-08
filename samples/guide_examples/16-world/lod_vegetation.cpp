// Guide chapter 16 «Открытый мир»: CDLOD terrain selection, vegetation scattering and culling.
#include <oxwald/world/terrain_gen.hpp>
#include <oxwald/world/terrain_lod.hpp>
#include <oxwald/world/vegetation.hpp>

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/trigonometric.hpp>

using namespace ox;
using namespace ox::world;

namespace {

Heightfield makeTerrain() {
    HeightfieldDesc d;
    d.resolution = 257;
    d.worldSize = 512.f;
    d.heightScale = 60.f;
    d.origin = {-256.f, -256.f};
    Heightfield hf(d);
    TerrainNoiseSettings ns;
    ns.fractal.frequency = 1.f / 200.f;
    ns.fractal.octaves = 4;
    generateNoise(hf, ns);
    return hf;
}

} // namespace

TEST(GuideWorldLod, SelectPatchesAroundTheCamera) {
    const Heightfield hf = makeTerrain();

    TerrainLodSettings ls;
    ls.leafNodeSize = 16;     // квадов на сторону узла LOD 0 (= размер сетки-меша)
    ls.lodCount = 4;          // LOD 0 — самый детальный, размер узла удваивается на уровень
    ls.viewDistance = 1000.f; // дальность самого грубого LOD
    const TerrainQuadtree tree(hf, ls);
    const TerrainGridMesh grid = generateTerrainGrid(ls.leafNodeSize, /*skirts*/ true); // один меш на все патчи
    EXPECT_EQ(grid.gridDim, 16u);

    const glm::vec3 camera{0.f, hf.sampleHeight({0.f, 0.f}) + 2.f, 0.f};
    TerrainSelection sel;
    tree.select({.cameraPosition = camera}, sel); // Frustum::infinite() по умолчанию
    ASSERT_FALSE(sel.patches.empty());
    EXPECT_FALSE(sel.truncated);
    EXPECT_GT(sel.patchesPerLod[0], 0u) << "у камеры — самый детальный LOD";

    // С фрустумом камеры (смотрит в −Z) патчей меньше.
    const glm::mat4 viewProj = glm::perspective(glm::radians(60.f), 16.f / 9.f, 0.1f, 2000.f) *
                               glm::lookAt(camera, camera + glm::vec3(0, 0, -1), glm::vec3(0, 1, 0));
    TerrainSelection visible;
    tree.select({.cameraPosition = camera, .frustum = Frustum::fromViewProjection(viewProj)}, visible);
    EXPECT_LT(visible.patches.size(), sel.patches.size());

    // Инстанс-буфер для рендера: 32 байта на патч.
    std::vector<TerrainPatchGpu> instances;
    for (const TerrainPatch& p : visible.patches) {
        instances.push_back(toGpu(p));
    }
    EXPECT_EQ(instances.size() * sizeof(TerrainPatchGpu), visible.patches.size() * 32u);
}

TEST(GuideWorldLod, PhysicsTilesFollowBrushEdits) {
    Heightfield hf = makeTerrain();
    // Коллайдеры ландшафта: тайлы по 64 квада (65 × 65 отсчётов, края общие с соседями).
    const std::vector<PhysicsHeightfieldTile> tiles = buildPhysicsTiles(hf, 64);
    EXPECT_EQ(tiles.size(), 16u); // 256 / 64 = 4 × 4
    EXPECT_EQ(tiles[0].sampleCount, 65u);

    // После правки ландшафта пересобираем только задетые тайлы.
    const IRect dirty{100, 100, 110, 110};
    const std::vector<glm::ivec2> touched = physicsTilesOverlapping(dirty, 64, hf.resolution());
    ASSERT_EQ(touched.size(), 1u);
    const PhysicsHeightfieldTile rebuilt = buildPhysicsTile(hf, touched[0].x, touched[0].y, 64);
    EXPECT_EQ(rebuilt.heights.size(), 65u * 65u);
}

TEST(GuideWorldVegetation, ScatterTreesAndGrass) {
    const Heightfield hf = makeTerrain();

    VegetationLayer pine;
    pine.name = "pine";
    pine.kind = VegetationKind::Tree;
    pine.prototype = 0;        // индекс меша/материала у рендера
    pine.minDistance = 6.f;    // радиус Poisson-диска: деревья не ближе 6 м
    pine.maxSlopeDeg = 25.f;
    pine.minScale = 0.8f;
    pine.maxScale = 1.3f;
    pine.boundingRadius = 6.f;
    pine.collider = true;      // деревьям — капсулы для физики
    pine.colliderRadius = 0.4f;
    pine.colliderHalfHeight = 3.f;
    pine.seed = 11;

    VegetationLayer grass;
    grass.name = "grass";
    grass.kind = VegetationKind::Grass;
    grass.prototype = 1;
    grass.minDistance = 1.5f;
    grass.maxSlopeDeg = 40.f;
    grass.alignToNormal = 1.f; // трава ложится по нормали склона
    grass.lod.cullDistance = 80.f;
    grass.lod.impostorDistance = 0.f; // без импостеров
    grass.seed = 12;

    const VegetationScatterer scatter({pine, grass});
    // Зона исключения: поляна под деревню (только для деревьев, бит 0).
    const ExclusionZone village{.shape = ExclusionZone::Shape::Circle, .center = {0.f, 0.f}, .halfExtents = {30.f, 0.f},
                                .layerMask = 1u << 0};
    ScatterContext ctx;
    ctx.heightfield = &hf;
    ctx.exclusions = std::span(&village, 1);

    // Чанк 128 × 128 м. Детерминированно и бесшовно: результат не зависит от нарезки на чанки.
    const VegetationChunk chunk = scatter.scatter({-64.f, -64.f}, 128.f, ctx);
    usize trees = 0;
    for (const VegetationInstance& v : chunk.instances) {
        if (v.layer == 0) {
            ++trees;
            EXPECT_GT(glm::length(glm::vec2(v.position.x, v.position.z)), 30.f) << "дерево в деревне";
            EXPECT_NEAR(v.position.y, hf.sampleHeight({v.position.x, v.position.z}), 1e-3f);
        }
    }
    EXPECT_GT(trees, 20u);
    EXPECT_EQ(chunk.colliders.size(), trees);

    // Повтор с тем же seed даёт ровно тот же результат.
    EXPECT_EQ(scatter.scatter({-64.f, -64.f}, 128.f, ctx).instances, chunk.instances);

    // Каллинг по ячейкам: фрустум + дистанция (трава пропадает дальше 80 м).
    std::vector<VisibleVegetationCell> visible;
    cullVegetationCells(chunk, scatter.layers(), Frustum::infinite(), {-60.f, 20.f, -60.f}, visible);
    for (const VisibleVegetationCell& c : visible) {
        if (chunk.cells[c.cell].layer == 1) {
            EXPECT_LE(c.distance, 80.f);
        }
    }

    // GPU-инстанс: 64 байта, матрица 3×4 в формате VkTransformMatrixKHR.
    const VegetationInstanceGpu gpu = toGpu(chunk.instances.front(), scatter.layers()[chunk.instances.front().layer]);
    EXPECT_EQ(sizeof(gpu), 64u);
}

TEST(GuideWorldVegetation, LodBands) {
    VegetationLodSettings s;
    s.lodDistances[0] = 20.f; // конец LOD 0
    s.lodDistances[1] = 50.f; // конец LOD 1, дальше LOD 2
    s.impostorDistance = 100.f;
    s.cullDistance = 300.f;
    s.fadeRange = 5.f;        // ширина кросс-фейда (дизеринг)
    EXPECT_EQ(selectVegetationLod(s, 10.f).lod, 0u);
    EXPECT_NEAR(selectVegetationLod(s, 17.5f).fade, 0.5f, 1e-5f); // посередине перехода 0 → 1
    EXPECT_EQ(selectVegetationLod(s, 80.f).lod, 2u);
    EXPECT_EQ(selectVegetationLod(s, 150.f).lod, VegetationLodResult::kImpostor);
    EXPECT_EQ(selectVegetationLod(s, 301.f).lod, VegetationLodResult::kCulled);
}
