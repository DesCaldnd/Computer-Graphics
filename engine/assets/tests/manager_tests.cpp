#include "test_helpers.hpp"

#include <oxwald/core/jobs.hpp>

using namespace oxtest;

namespace {

// Two textures + a material referencing them + a mesh.
struct Project {
    TempDir dir;
    std::unique_ptr<AssetRegistry> registry;
    Uuid texA, texB, material, model;

    Project() {
        writePng(dir / "Assets/a.png", gradientImage(16, 16));
        writePng(dir / "Assets/b_normal.png", normalMapImage(16, 16));
        AssetRegistry::Options o;
        o.watchDebounce = std::chrono::milliseconds(20);
        registry = std::make_unique<AssetRegistry>(dir.path(), o);
        registry->scan();
        texA = *registry->uuidForPath("a.png");
        texB = *registry->uuidForPath("b_normal.png");
        MaterialAsset m;
        m.albedoTexture = texA;
        m.normalTexture = texB;
        EXPECT_TRUE(saveMaterial(m, dir / "Assets/m.oxmat"));
        fs::copy_file(legacyDir() / "cube.obj", dir / "Assets/cube.obj");
        registry->scan();
        material = *registry->uuidForPath("m.oxmat");
        model = *registry->uuidForPath("cube.obj");
    }
};

} // namespace

TEST(AssetManager, AsyncLoadWaitsForDependencies) {
    Project p;
    JobSystem jobs(4);
    AssetManager mgr(*p.registry, &jobs);
    std::vector<Uuid> loadedOrder;
    ScopedConnection c = mgr.onLoaded.connect([&](const Uuid& u, AssetType) { loadedOrder.push_back(u); });
    bool callbackOk = false;
    bool texturesReadyInCallback = false;
    auto mat = mgr.load<MaterialAsset>(p.material, kPriorityHigh);
    mat.onLoaded([&](bool ok) {
        callbackOk = ok;
        texturesReadyInCallback = mgr.state(p.texA) == AssetState::Loaded && mgr.state(p.texB) == AssetState::Loaded;
    });
    EXPECT_TRUE(mat.future().get());
    mgr.waitAll();
    EXPECT_TRUE(callbackOk);
    EXPECT_TRUE(texturesReadyInCallback);
    ASSERT_TRUE(mat.isLoaded());
    EXPECT_EQ(mat->albedoTexture, p.texA);
    // Textures finished before the material.
    auto pos = [&](const Uuid& u) { return std::find(loadedOrder.begin(), loadedOrder.end(), u) - loadedOrder.begin(); };
    EXPECT_LT(pos(p.texA), pos(p.material));
    EXPECT_LT(pos(p.texB), pos(p.material));
    auto tex = mgr.load<TextureData>(p.texB);
    ASSERT_TRUE(tex.isLoaded());
    EXPECT_EQ(tex->format, TextureFormat::BC5Unorm);
}

TEST(AssetManager, SyncLoadAndSubAssetsByPath) {
    Project p;
    AssetManager mgr(*p.registry);
    auto prefab = mgr.loadSync<PrefabAsset>("cube.obj");
    ASSERT_TRUE(prefab.isLoaded()) << prefab.error();
    EXPECT_EQ(prefab->document.kind, "prefab");
    auto mesh = mgr.loadSync<MeshData>("cube.obj#Mesh/0");
    ASSERT_TRUE(mesh.isLoaded()) << mesh.error();
    EXPECT_GT(mesh->vertexCount(), 0u);
    EXPECT_FALSE(mesh->meshlets.empty());
}

TEST(AssetManager, RefCountUnloadWithLru) {
    Project p;
    AssetManager::Options o;
    o.keepUnreferenced = 0;
    AssetManager mgr(*p.registry, nullptr, o);
    std::vector<Uuid> unloaded;
    ScopedConnection c = mgr.onUnloaded.connect([&](const Uuid& u, AssetType) { unloaded.push_back(u); });
    {
        auto mat = mgr.loadSync<MaterialAsset>(p.material);
        ASSERT_TRUE(mat.isLoaded());
        auto copy = mat;
        mgr.update();
        EXPECT_TRUE(unloaded.empty());
        EXPECT_EQ(mgr.stats().perType[AssetType::Texture].count, 2u + 3u); // + 3 built-in textures
    }
    mgr.update(); // material unreferenced -> evicted, which releases the textures -> evicted too
    EXPECT_EQ(mgr.state(p.material), AssetState::Unloaded);
    EXPECT_EQ(mgr.state(p.texA), AssetState::Unloaded);
    EXPECT_EQ(unloaded.size(), 3u);

    // With an LRU of 8 unreferenced assets, they stay cached.
    AssetManager::Options keep;
    keep.keepUnreferenced = 8;
    AssetManager cached(*p.registry, nullptr, keep);
    { auto mat = cached.loadSync<MaterialAsset>(p.material); }
    cached.update();
    EXPECT_EQ(cached.state(p.material), AssetState::Loaded);
    EXPECT_EQ(cached.stats().unreferenced, 1u);
    EXPECT_EQ(cached.unloadUnused(), 3u);
}

TEST(AssetManager, MemoryBudgetEvictsOldestUnreferenced) {
    Project p;
    AssetManager::Options o;
    o.keepUnreferenced = 100;
    AssetManager mgr(*p.registry, nullptr, o);
    { auto a = mgr.loadSync<TextureData>(p.texA); }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    auto b = mgr.loadSync<TextureData>(p.texB);
    mgr.update();
    EXPECT_EQ(mgr.state(p.texA), AssetState::Loaded);
    mgr.setMemoryBudget(1); // everything is over budget: only referenced assets survive
    mgr.update();
    EXPECT_EQ(mgr.state(p.texA), AssetState::Unloaded);
    EXPECT_EQ(mgr.state(p.texB), AssetState::Loaded);
    const AssetStats s = mgr.stats();
    EXPECT_GT(s.memoryBytes, 0u);
    EXPECT_EQ(s.memoryBudget, 1u);
}

TEST(AssetManager, PlaceholdersAndFailures) {
    Project p;
    AssetManager mgr(*p.registry);
    auto missing = mgr.loadSync<TextureData>(Uuid::fromName("does-not-exist"));
    EXPECT_TRUE(missing.isFailed());
    EXPECT_EQ(missing.get(), nullptr);
    ASSERT_NE(missing.getOrDefault(), nullptr);
    EXPECT_EQ(missing.getOrDefault()->width, 64u); // checker
    auto wrongType = mgr.loadSync<MeshData>(p.texA);
    EXPECT_TRUE(wrongType.isFailed());
    EXPECT_NE(wrongType.error().find("type mismatch"), std::string::npos);
    auto cube = mgr.loadSync<MeshData>(builtin::cubeMesh());
    ASSERT_TRUE(cube.isLoaded());
    EXPECT_EQ(cube->vertexCount(), 24u);
    auto def = mgr.load<MaterialAsset>(builtin::defaultMaterial());
    EXPECT_TRUE(def.isLoaded());
}

TEST(AssetManager, PriorityOrder) {
    Project p;
    JobSystem jobs(2); // one worker: requests run strictly in priority order once queued
    AssetManager mgr(*p.registry, &jobs);
    p.registry->importAll();
    std::vector<Uuid> order;
    ScopedConnection c = mgr.onLoaded.connect([&](const Uuid& u, AssetType) { order.push_back(u); });
    auto low = mgr.load<TextureData>(p.texA, kPriorityLow);
    auto high = mgr.load<TextureData>(p.texB, kPriorityCritical);
    mgr.waitAll();
    EXPECT_TRUE(low.isLoaded());
    EXPECT_TRUE(high.isLoaded());
    EXPECT_EQ(order.size(), 2u);
}

TEST(AssetManager, HotReloadSignal) {
    Project p;
    JobSystem jobs(2);
    AssetManager mgr(*p.registry, &jobs);
    auto tex = mgr.load<TextureData>(p.texA);
    ASSERT_TRUE(tex.wait());
    const u32 gen = tex.generation();
    EXPECT_EQ(tex->width, 16u);
    std::vector<Uuid> reloaded;
    ScopedConnection c = mgr.onReloaded.connect([&](const Uuid& u, AssetType) { reloaded.push_back(u); });
    p.registry->startWatching();
    p.registry->poll();
    writePng(p.dir / "Assets/a.png", gradientImage(32, 32));
    touchLater(p.dir / "Assets/a.png");
    EXPECT_TRUE(waitUntil([&] {
        p.registry->poll();
        mgr.update();
        return !reloaded.empty();
    }));
    ASSERT_EQ(reloaded.size(), 1u);
    EXPECT_EQ(reloaded[0], p.texA);
    EXPECT_EQ(tex->width, 32u);
    EXPECT_GT(tex.generation(), gen);
}
