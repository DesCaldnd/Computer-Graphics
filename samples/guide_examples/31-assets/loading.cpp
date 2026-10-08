// Глава 31: асинхронная загрузка — AssetManager, AssetHandle, приоритеты, плейсхолдеры, бюджет памяти,
// hot reload (docs/guide/31-assets.md).
#include "project_fixture.hpp"

#include <oxwald/assets/assets.hpp>
#include <oxwald/core/jobs.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

using namespace ox;
using namespace ox::assets;
using namespace guide31;

namespace {

// Проект: две текстуры, материал на них и модель.
struct LoadingProject {
    TempProject project;
    std::unique_ptr<AssetRegistry> registry;
    Uuid albedo, normal, material;

    LoadingProject() {
        writeGradientPng(project.asset("Textures/rock.png"), 32, 32);
        writeNormalPng(project.asset("Textures/rock_normal.png"), 32, 32);
        writeHillObj(project.asset("Models"));
        AssetRegistry::Options options;
        options.watchDebounce = std::chrono::milliseconds(20);
        registry = std::make_unique<AssetRegistry>(project.root(), options);
        registry->scan();
        albedo = *registry->uuidForPath("Textures/rock.png");
        normal = *registry->uuidForPath("Textures/rock_normal.png");
        MaterialAsset m;
        m.albedoTexture = albedo;
        m.normalTexture = normal;
        m.roughness = 0.8f;
        EXPECT_TRUE(saveMaterial(m, project.asset("Materials/rock.oxmat")));
        registry->scan();
        material = *registry->uuidForPath("Materials/rock.oxmat");
    }
};

} // namespace

TEST(GuideAssetsLoading, AsyncLoadWithDependencies) {
    LoadingProject p;
    JobSystem jobs(2);
    AssetManager assets(*p.registry, &jobs, {.memoryBudget = 512u << 20});

    std::vector<Uuid> order;
    ScopedConnection c = assets.onLoaded.connect([&](const Uuid& id, AssetType) { order.push_back(id); });

    AssetHandle<MaterialAsset> mat = assets.load<MaterialAsset>("Materials/rock.oxmat", kPriorityHigh);
    bool texturesReady = false;
    mat.onLoaded([&](bool ok) {   // главный поток, внутри assets.update(); текстуры к этому моменту загружены
        texturesReady = ok && assets.state(p.albedo) == AssetState::Loaded && assets.state(p.normal) == AssetState::Loaded;
    });
    // Пока грузится — get() == nullptr, getOrDefault() даёт встроенный плейсхолдер.
    EXPECT_NE(mat.getOrDefault(), nullptr);

    // В игре: assets.update() раз в кадр. В тесте ждём всё сразу.
    assets.waitAll();
    ASSERT_TRUE(mat.isLoaded()) << mat.error();
    EXPECT_TRUE(texturesReady);
    EXPECT_FLOAT_EQ(mat->roughness, 0.8f);
    // Зависимости (текстуры) завершились раньше материала.
    auto pos = [&](const Uuid& id) { return std::find(order.begin(), order.end(), id) - order.begin(); };
    EXPECT_LT(pos(p.albedo), pos(p.material));
    EXPECT_LT(pos(p.normal), pos(p.material));

    const AssetStats stats = assets.stats();
    EXPECT_GE(stats.loaded, 3u);
    EXPECT_EQ(stats.memoryBudget, 512u << 20);
}

TEST(GuideAssetsLoading, SyncLoadSubAssetsAndFuture) {
    LoadingProject p;
    AssetManager assets(*p.registry);   // без JobSystem — всё синхронно

    AssetHandle<MeshData> mesh = assets.loadSync<MeshData>("Models/hill.obj#Mesh/0");
    ASSERT_TRUE(mesh.isLoaded()) << mesh.error();
    EXPECT_GT(mesh->vertexCount(), 0u);
    AssetHandle<PrefabAsset> prefab = assets.loadSync<PrefabAsset>("Models/hill.obj");
    ASSERT_TRUE(prefab.isLoaded());
    EXPECT_EQ(prefab->document.kind, "prefab");   // ox::instantiatePrefab(world, prefab->document)

    // future() — точка стыковки с корутинами (глава 08).
    std::shared_future<bool> f = assets.load<TextureData>(p.albedo).future();
    EXPECT_TRUE(f.get());
}

TEST(GuideAssetsLoading, PlaceholdersAndErrors) {
    LoadingProject p;
    AssetManager assets(*p.registry);
    auto missing = assets.loadSync<TextureData>(Uuid::fromName("нет такого ассета"));
    EXPECT_TRUE(missing.isFailed());
    EXPECT_EQ(missing.get(), nullptr);
    ASSERT_NE(missing.getOrDefault(), nullptr);
    EXPECT_EQ(missing.getOrDefault()->width, 64u);   // шахматная текстура 64×64

    auto wrong = assets.loadSync<MeshData>(p.albedo);   // это текстура, а не меш
    EXPECT_TRUE(wrong.isFailed());
    EXPECT_NE(wrong.error().find("type mismatch"), std::string::npos);

    auto cube = assets.loadSync<MeshData>(builtin::cubeMesh());   // встроенные ассеты всегда в памяти
    ASSERT_TRUE(cube.isLoaded());
    EXPECT_EQ(cube->vertexCount(), 24u);
}

TEST(GuideAssetsLoading, RefCountingAndLru) {
    LoadingProject p;
    AssetManager::Options options;
    options.keepUnreferenced = 0;   // не держать в кэше ассеты без ссылок
    AssetManager assets(*p.registry, nullptr, options);
    {
        auto mat = assets.loadSync<MaterialAsset>(p.material);
        auto copy = mat;   // копии делят одну загрузку
        assets.update();
        EXPECT_EQ(assets.state(p.material), AssetState::Loaded);
    }   // последняя ссылка отпущена
    assets.update();   // материал выгружен, за ним — его текстуры
    EXPECT_EQ(assets.state(p.material), AssetState::Unloaded);
    EXPECT_EQ(assets.state(p.albedo), AssetState::Unloaded);

    AssetManager cached(*p.registry, nullptr, {.keepUnreferenced = 8});
    { auto mat = cached.loadSync<MaterialAsset>(p.material); }
    cached.update();
    EXPECT_EQ(cached.state(p.material), AssetState::Loaded);   // остался в LRU-кэше
    EXPECT_EQ(cached.unloadUnused(), 3u);                       // материал + 2 текстуры
}

TEST(GuideAssetsLoading, HotReload) {
    LoadingProject p;
    JobSystem jobs(2);
    AssetManager assets(*p.registry, &jobs);
    auto tex = assets.load<TextureData>(p.albedo);
    ASSERT_TRUE(tex.wait());
    const u32 generation = tex.generation();
    EXPECT_EQ(tex->width, 32u);

    std::vector<Uuid> reloaded;
    ScopedConnection c = assets.onReloaded.connect([&](const Uuid& id, AssetType) { reloaded.push_back(id); });
    p.registry->startWatching();
    p.registry->poll();   // базовая точка

    writeGradientPng(p.project.asset("Textures/rock.png"), 64, 64);   // художник сохранил новую версию
    touchLater(p.project.asset("Textures/rock.png"));
    ASSERT_TRUE(waitUntil([&] {
        p.registry->poll();   // реимпорт изменённых исходников → AssetManager перезагружает в фоне
        assets.update();      // подмена данных и сигнал onReloaded
        return !reloaded.empty();
    }));
    EXPECT_EQ(reloaded.front(), p.albedo);
    EXPECT_EQ(tex->width, 64u);                // тот же handle — новые данные
    EXPECT_GT(tex.generation(), generation);   // GPU-кэш сравнивает generation и перезаливает
}
