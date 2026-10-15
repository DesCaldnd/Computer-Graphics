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

// A meta that exists but does not parse (mid-write by git or another process, or broken by hand) is never replaced by
// a fresh random UUID: that would silently break every reference to the asset (the showcase guard.oxbt lost its
// committed id this way).
TEST(AssetRegistry, UnreadableMetaIsNeverReplaced) {
    TempDir dir;
    writeText(dir / "Assets/Scripts/main.lua", "print('hi')");
    const fs::path metaPath = dir / "Assets/Scripts/main.lua.meta";
    AssetRegistry reg(dir.path());
    reg.scan();
    const Uuid id = reg.uuidForPath("Scripts/main.lua").value_or(Uuid{});
    ASSERT_TRUE(id.isValid());
    const std::string good = readText(metaPath);
    const std::string broken = good.substr(0, good.size() / 2); // a truncated, half-written meta
    writeText(metaPath, broken);

    const ScanResult r = reg.scan();
    EXPECT_EQ(r.metasCreated, 0u);
    EXPECT_EQ(r.removed, 0u) << "a known asset keeps its identity while its meta is unreadable";
    EXPECT_EQ(reg.uuidForPath("Scripts/main.lua"), id);
    EXPECT_EQ(readText(metaPath), broken) << "the meta file must be left alone";
    {
        AssetRegistry fresh(dir.path());
        EXPECT_EQ(fresh.scan().metasCreated, 0u);
        EXPECT_FALSE(fresh.uuidForPath("Scripts/main.lua")) << "skipped until its meta is readable";
        EXPECT_EQ(readText(metaPath), broken);
    }
    writeText(metaPath, good); // the writer finished (or the user fixed it)
    AssetRegistry again(dir.path());
    again.scan();
    EXPECT_EQ(again.uuidForPath("Scripts/main.lua"), id);
}

// A model that names its texture with a different letter case resolves the same file on a case-insensitive file
// system: that is the known asset, not a copy that needs a new identity (its meta must not be rewritten).
TEST(AssetRegistry, DifferentlySpelledPathToKnownFileKeepsItsUuid) {
    TempDir dir;
    const fs::path gltf = writeTestGltf(dir / "Assets/Models", "box");
    std::string json = readText(gltf);
    for (usize p = json.find("box_albedo.png"); p != std::string::npos; p = json.find("box_albedo.png"))
        json.replace(p, 14, "BOX_Albedo.png");
    writeText(gltf, json);
    if (!fs::exists(dir / "Assets/Models/BOX_Albedo.png")) GTEST_SKIP() << "case-sensitive file system";
    AssetRegistry reg(dir.path());
    reg.scan();
    const Uuid albedo = reg.uuidForPath("Models/box_albedo.png").value_or(Uuid{});
    ASSERT_TRUE(albedo.isValid());
    const Uuid model = *reg.uuidForPath("Models/box.gltf");
    ASSERT_TRUE(reg.import(model));
    EXPECT_EQ(reg.uuidForPath("Models/box_albedo.png"), albedo);
    EXPECT_EQ(readMeta(dir / "Assets/Models/box_albedo.png.meta")->uuid, albedo);
    const auto mat0 = reg.uuidForPath("Models/box.gltf#Material/0");
    ASSERT_TRUE(mat0);
    const auto deps = reg.dependencies(*mat0);
    EXPECT_NE(std::find(deps.begin(), deps.end(), albedo), deps.end()) << "the material references the known texture";
    AssetRegistry again(dir.path());
    EXPECT_EQ(again.scan().duplicatesFixed, 0u);
    EXPECT_EQ(again.uuidForPath("Models/box_albedo.png"), albedo);
}

// A registry without the importer for a file type (oxpack has no gameplay importers for .oxbt) must keep the meta
// of such a file: the source is still there, so the meta is not an orphan (oxpack deleted AI/guard.oxbt.meta).
TEST(AssetRegistry, MetaOfSourceWithoutImporterIsKept) {
    TempDir dir;
    writeText(dir / "Assets/AI/guard.oxbt", "{\"root\": {}}");
    writeText(dir / "Assets/AI/guard.oxbt.meta",
              "{\n  \"formatVersion\": 1,\n  \"uuid\": \"f0eadad5-9e1d-8949-9966-c395d546d9a1\",\n"
              "  \"importer\": \"behavior_tree\",\n  \"importerVersion\": 1,\n  \"settings\": {}\n}\n");
    writeText(dir / "Assets/Scripts/gone.lua", "print('gone')");
    {
        AssetRegistry writer(dir.path());
        writer.scan();
    }
    ASSERT_TRUE(fs::exists(dir / "Assets/Scripts/gone.lua.meta"));
    fs::remove(dir / "Assets/Scripts/gone.lua");
    AssetRegistry reg(dir.path()); // default options: writeMetas and deleteOrphanMetas on
    const ScanResult r = reg.scan();
    EXPECT_EQ(r.found, 0u);
    EXPECT_FALSE(reg.uuidForPath("AI/guard.oxbt")) << "no importer for .oxbt in this registry";
    EXPECT_TRUE(fs::exists(dir / "Assets/AI/guard.oxbt.meta")) << "the source exists: its meta is not an orphan";
    EXPECT_FALSE(fs::exists(dir / "Assets/Scripts/gone.lua.meta")) << "a real orphan meta is still deleted";
}

// writeMetas = false (oxshowcase_generate --check): the source tree stays untouched; a meta-less source gets an
// in-memory identity that is stable across rescans.
TEST(AssetRegistry, ReadOnlyRegistryNeverWritesMetas) {
    TempDir dir;
    writeText(dir / "Assets/Scripts/main.lua", "print('hi')");
    writeText(dir / "Assets/Scripts/known.lua", "print('known')");
    {
        AssetRegistry writer(dir.path());
        writer.scan();
    }
    fs::remove(dir / "Assets/Scripts/main.lua.meta");
    const Uuid known = readMeta(dir / "Assets/Scripts/known.lua.meta")->uuid;
    fs::copy_file(dir / "Assets/Scripts/known.lua.meta", dir / "Assets/Scripts/orphan.lua.meta"); // source missing
    AssetRegistry::Options o;
    o.writeMetas = false;
    AssetRegistry reg(dir.path(), o);
    const ScanResult r = reg.scan();
    EXPECT_EQ(r.metasCreated, 1u);
    const Uuid id = reg.uuidForPath("Scripts/main.lua").value_or(Uuid{});
    EXPECT_TRUE(id.isValid());
    EXPECT_EQ(reg.uuidForPath("Scripts/known.lua"), known);
    EXPECT_FALSE(fs::exists(dir / "Assets/Scripts/main.lua.meta"));
    EXPECT_TRUE(fs::exists(dir / "Assets/Scripts/orphan.lua.meta")) << "orphan metas are not deleted either";
    reg.scan();
    EXPECT_EQ(reg.uuidForPath("Scripts/main.lua"), id) << "stable in memory";
    EXPECT_FALSE(reg.setSettings(known, {{"x", 1}}));
    EXPECT_FALSE(fs::exists(dir / "Assets/Scripts/main.lua.meta"));
}
