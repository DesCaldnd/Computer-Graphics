#include "test_helpers.hpp"

using namespace oxtest;

namespace {

AssetRegistry::Options quietOptions() {
    AssetRegistry::Options o;
    o.watchDebounce = std::chrono::milliseconds(20);
    return o;
}

} // namespace

TEST(AssetRegistry, ScanCreatesMetasWithStableUuids) {
    TempDir dir;
    writePng(dir / "Assets/Textures/rock.png", gradientImage(8, 8));
    writeText(dir / "Assets/Scripts/main.lua", "print('hi')");
    writeText(dir / "Assets/readme.txt", "no importer"); // ignored
    Uuid rock, script;
    {
        AssetRegistry reg(dir.path());
        const ScanResult r = reg.scan();
        EXPECT_EQ(r.found, 2u);
        EXPECT_EQ(r.metasCreated, 2u);
        ASSERT_TRUE(fs::exists(dir / "Assets/Textures/rock.png.meta"));
        EXPECT_FALSE(fs::exists(dir / "Assets/readme.txt.meta"));
        rock = reg.uuidForPath("Textures/rock.png").value_or(Uuid{});
        script = reg.uuidForPath("Scripts/main.lua").value_or(Uuid{});
        EXPECT_TRUE(rock.isValid());
        EXPECT_TRUE(script.isValid());
        auto meta = readMeta(dir / "Assets/Textures/rock.png.meta");
        ASSERT_TRUE(meta);
        EXPECT_EQ(meta->uuid, rock);
        EXPECT_EQ(meta->importer, "texture");
        EXPECT_EQ(meta->settings.value("type", std::string{}), "Color");
        // Rescan in the same instance: nothing new.
        EXPECT_EQ(reg.scan().metasCreated, 0u);
        EXPECT_EQ(reg.uuidForPath("Textures/rock.png"), rock);
    }
    AssetRegistry reg2(dir.path());
    reg2.scan();
    EXPECT_EQ(reg2.uuidForPath("Textures/rock.png"), rock);
    EXPECT_EQ(reg2.uuidForPath("Scripts/main.lua"), script);
}

TEST(AssetRegistry, TextureDefaultSettingsFromFileName) {
    TempDir dir;
    writePng(dir / "Assets/brick_normal.png", normalMapImage(8, 8));
    writePng(dir / "Assets/brick_orm.png", gradientImage(8, 8));
    AssetRegistry reg(dir.path());
    reg.scan();
    EXPECT_EQ(reg.meta(*reg.uuidForPath("brick_normal.png"))->settings["type"], "Normal");
    EXPECT_EQ(reg.meta(*reg.uuidForPath("brick_orm.png"))->settings["type"], "Linear");
}

TEST(AssetRegistry, DetectsMoveWithMeta) {
    TempDir dir;
    writePng(dir / "Assets/a.png", gradientImage(8, 8));
    AssetRegistry reg(dir.path(), quietOptions());
    reg.scan();
    const Uuid id = *reg.uuidForPath("a.png");
    ASSERT_TRUE(reg.import(id));
    std::string from, to;
    ScopedConnection c = reg.onMoved.connect([&](const Uuid& u, const std::string& f, const std::string& t) {
        EXPECT_EQ(u, id);
        from = f;
        to = t;
    });
    fs::create_directories(dir / "Assets/Moved");
    fs::rename(dir / "Assets/a.png", dir / "Assets/Moved/b.png");
    fs::rename(dir / "Assets/a.png.meta", dir / "Assets/Moved/b.png.meta");
    const ScanResult r = reg.scan();
    EXPECT_EQ(r.moved, 1u);
    EXPECT_EQ(from, "a.png");
    EXPECT_EQ(to, "Moved/b.png");
    EXPECT_EQ(reg.uuidForPath("Moved/b.png"), id);
    EXPECT_FALSE(reg.uuidForPath("a.png"));
    EXPECT_FALSE(reg.needsImport(id)); // artifacts survive the move
}

TEST(AssetRegistry, DetectsMoveWithoutMetaByContent) {
    TempDir dir;
    writePng(dir / "Assets/a.png", gradientImage(8, 8));
    writePng(dir / "Assets/other.png", gradientImage(4, 4));
    AssetRegistry reg(dir.path());
    reg.scan();
    const Uuid id = *reg.uuidForPath("a.png");
    ASSERT_TRUE(reg.import(id)); // records the content hash
    fs::rename(dir / "Assets/a.png", dir / "Assets/renamed.png"); // meta left behind (external tool)
    const ScanResult r = reg.scan();
    EXPECT_EQ(r.metasCreated, 0u);
    EXPECT_EQ(reg.uuidForPath("renamed.png"), id);
    EXPECT_TRUE(fs::exists(dir / "Assets/renamed.png.meta"));
    EXPECT_FALSE(fs::exists(dir / "Assets/a.png.meta"));
}

TEST(AssetRegistry, CopiedMetaGetsNewUuid) {
    TempDir dir;
    writePng(dir / "Assets/a.png", gradientImage(8, 8));
    AssetRegistry reg(dir.path());
    reg.scan();
    const Uuid id = *reg.uuidForPath("a.png");
    fs::copy_file(dir / "Assets/a.png", dir / "Assets/a_copy.png");
    fs::copy_file(dir / "Assets/a.png.meta", dir / "Assets/a_copy.png.meta");
    const ScanResult r = reg.scan();
    EXPECT_EQ(r.duplicatesFixed, 1u);
    EXPECT_EQ(reg.uuidForPath("a.png"), id);
    const auto copy = reg.uuidForPath("a_copy.png");
    ASSERT_TRUE(copy);
    EXPECT_NE(*copy, id);
}

TEST(AssetRegistry, RemovedAssetDropsMetaAndCache) {
    TempDir dir;
    writePng(dir / "Assets/a.png", gradientImage(8, 8));
    AssetRegistry reg(dir.path());
    reg.scan();
    const Uuid id = *reg.uuidForPath("a.png");
    ASSERT_TRUE(reg.import(id));
    const auto artifact = reg.info(id)->artifact;
    ASSERT_TRUE(fs::exists(artifact));
    fs::remove(dir / "Assets/a.png");
    const ScanResult r = reg.scan();
    EXPECT_EQ(r.removed, 1u);
    EXPECT_FALSE(fs::exists(dir / "Assets/a.png.meta"));
    EXPECT_FALSE(fs::exists(artifact));
    EXPECT_FALSE(reg.info(id));
}

TEST(AssetRegistry, ImportCachesAndReimportsOnSettingsChange) {
    TempDir dir;
    writePng(dir / "Assets/a.png", gradientImage(16, 16));
    AssetRegistry reg(dir.path());
    reg.scan();
    const Uuid id = *reg.uuidForPath("a.png");
    EXPECT_TRUE(reg.needsImport(id));
    ImportStats s = reg.importAll();
    EXPECT_EQ(s.imported, 1u);
    EXPECT_FALSE(reg.needsImport(id));
    EXPECT_EQ(reg.importAll().upToDate, 1u);
    auto rec = reg.record(id);
    ASSERT_TRUE(rec);
    EXPECT_EQ(rec->type, AssetType::Texture);
    auto tex = deserializeTexture(*reg.readArtifact(id));
    ASSERT_TRUE(tex);
    EXPECT_EQ(tex->format, TextureFormat::BC7Srgb);

    ASSERT_TRUE(reg.setSettings(id, {{"compression", "Uncompressed"}}));
    auto tex2 = deserializeTexture(*reg.readArtifact(id));
    ASSERT_TRUE(tex2);
    EXPECT_EQ(tex2->format, TextureFormat::RGBA8Srgb);
    // New instance: cache is reused (record + hashes on disk).
    AssetRegistry reg2(dir.path());
    reg2.scan();
    EXPECT_FALSE(reg2.needsImport(id));
}

TEST(AssetRegistry, ModelImportProducesSubAssetsAndDependencies) {
    TempDir dir;
    writeTestGltf(dir / "Assets/Models", "box");
    AssetRegistry reg(dir.path());
    reg.scan();
    const Uuid model = *reg.uuidForPath("Models/box.gltf");
    ASSERT_TRUE(reg.import(model));
    auto info = reg.info(model);
    ASSERT_TRUE(info);
    EXPECT_EQ(info->type, AssetType::Prefab);
    // 1 mesh + 2 materials (textures are separate files).
    EXPECT_EQ(info->subAssets.size(), 3u);
    const auto mesh = reg.uuidForPath("Models/box.gltf#Mesh/0");
    const auto mat0 = reg.uuidForPath("Models/box.gltf#Material/0");
    ASSERT_TRUE(mesh && mat0);
    EXPECT_EQ(reg.info(*mesh)->type, AssetType::Mesh);
    const auto albedo = reg.uuidForPath("Models/box_albedo.png");
    const auto normal = reg.uuidForPath("Models/box_n.png");
    ASSERT_TRUE(albedo && normal);
    auto deps = reg.dependencies(*mat0);
    EXPECT_NE(std::find(deps.begin(), deps.end(), *albedo), deps.end());
    EXPECT_NE(std::find(deps.begin(), deps.end(), *normal), deps.end());
    // The model hinted the normal map's import settings.
    EXPECT_EQ(reg.meta(*normal)->settings["type"], "Normal");
    auto dependents = reg.dependents(*albedo);
    EXPECT_NE(std::find(dependents.begin(), dependents.end(), *mat0), dependents.end());
    auto closure = reg.collectDependencies(std::vector<Uuid>{model});
    EXPECT_GE(closure.size(), 5u); // prefab, mesh, 2 materials, 2 textures
    // Prefab contains a MeshRenderer pointing at the mesh sub-asset.
    auto doc = serial::decodeBinary(*reg.readArtifact(model));
    ASSERT_TRUE(doc);
    EXPECT_EQ(doc->kind, "prefab");
    auto refs = collectUuidRefs(doc->root);
    EXPECT_NE(std::find(refs.begin(), refs.end(), *mesh), refs.end());
}

TEST(AssetRegistry, HotReloadSignalAfterSourceEdit) {
    TempDir dir;
    writePng(dir / "Assets/a.png", gradientImage(8, 8));
    AssetRegistry reg(dir.path(), quietOptions());
    reg.scan();
    const Uuid id = *reg.uuidForPath("a.png");
    ASSERT_TRUE(reg.import(id));
    reg.startWatching();
    reg.poll(); // establish the baseline
    std::vector<Uuid> reimported;
    ScopedConnection c = reg.onReimported.connect([&](const Uuid& u) { reimported.push_back(u); });
    writePng(dir / "Assets/a.png", gradientImage(16, 16));
    touchLater(dir / "Assets/a.png");
    EXPECT_TRUE(waitUntil([&] {
        reg.poll();
        return !reimported.empty();
    }));
    ASSERT_FALSE(reimported.empty());
    EXPECT_EQ(reimported[0], id);
    auto tex = deserializeTexture(*reg.readArtifact(id));
    ASSERT_TRUE(tex);
    EXPECT_EQ(tex->width, 16u);
}

TEST(AssetRegistry, PassthroughImporters) {
    TempDir dir;
    // 16-bit PCM WAV, 1 channel, 8 kHz, 800 frames.
    std::vector<std::byte> wav(44 + 1600);
    auto put32 = [&](usize o, u32 v) { std::memcpy(wav.data() + o, &v, 4); };
    auto put16 = [&](usize o, u16 v) { std::memcpy(wav.data() + o, &v, 2); };
    std::memcpy(wav.data(), "RIFF", 4);
    put32(4, u32(wav.size() - 8));
    std::memcpy(wav.data() + 8, "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);
    put16(22, 1);
    put32(24, 8000);
    put32(28, 16000);
    put16(32, 2);
    put16(34, 16);
    std::memcpy(wav.data() + 36, "data", 4);
    put32(40, 1600);
    writeBytes(dir / "Assets/beep.wav", wav);
    std::vector<std::byte> height(64 * 64 * 2, std::byte{7});
    writeBytes(dir / "Assets/terrain.r16", height);
    writeText(dir / "Assets/font.ttf", "not really a font");
    AssetRegistry reg(dir.path());
    reg.scan();
    EXPECT_EQ(reg.importAll().failed, 0u);
    auto audio = deserializeBlob(*reg.readArtifact(*reg.uuidForPath("beep.wav")));
    ASSERT_TRUE(audio);
    EXPECT_EQ(audio->type, AssetType::Audio);
    EXPECT_EQ(audio->info["sampleRate"], 8000);
    EXPECT_EQ(audio->info["frames"], 800);
    EXPECT_NEAR(audio->info["duration"].get<f64>(), 0.1, 1e-9);
    EXPECT_EQ(audio->data.size(), wav.size());
    auto hm = deserializeBlob(*reg.readArtifact(*reg.uuidForPath("terrain.r16")));
    ASSERT_TRUE(hm);
    EXPECT_EQ(hm->info["width"], 64);
    EXPECT_EQ(reg.record(*reg.uuidForPath("font.ttf"))->type, AssetType::Font);
}
