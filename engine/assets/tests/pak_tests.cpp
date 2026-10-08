#include "test_helpers.hpp"

#include <oxwald/core/jobs.hpp>
#include <oxwald/core/serial/format.hpp>
#include <oxwald/scene/scene.hpp>
#include <oxwald/scene/scene_serializer.hpp>

#include <set>

using namespace oxtest;

TEST(Pak, WriteReadMountAndIntegrity) {
    TempDir dir;
    PakWriter w;
    std::string big(100000, 'x');
    for (usize i = 0; i < big.size(); i += 7) big[i] = char('a' + i % 26);
    w.add("data/big.txt", std::span(reinterpret_cast<const std::byte*>(big.data()), big.size()), PakCompression::Zstd);
    w.add("data/small.bin", std::vector<std::byte>{std::byte{1}, std::byte{2}, std::byte{3}});
    w.add("root.txt", std::span(reinterpret_cast<const std::byte*>("hello"), 5));
    const fs::path pak = dir / "test.oxpak";
    ASSERT_TRUE(w.write(pak));
    auto reader = PakReader::open(pak);
    ASSERT_TRUE(reader) << reader.error().message;
    EXPECT_EQ((*reader)->entries().size(), 3u);
    const auto* e = (*reader)->find("data/big.txt");
    ASSERT_NE(e, nullptr);
    if (pakCompressionAvailable()) {
        EXPECT_EQ(e->compression, PakCompression::Zstd);
        EXPECT_LT(e->storedSize, e->size);
    }
    auto bytes = (*reader)->read(*e);
    ASSERT_TRUE(bytes);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size()), big);
    // Alignment of uncompressed entries (mmap friendly).
    EXPECT_EQ((*reader)->find("data/small.bin")->offset % 16, 0u);
    EXPECT_TRUE((*reader)->view(*(*reader)->find("root.txt")).has_value());
    auto range = (*reader)->readRange(*e, 7, 3);
    ASSERT_TRUE(range);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(range->data()), 3), big.substr(7, 3));

    Vfs vfs;
    vfs.mount("game", std::make_unique<PakMountSource>(*reader));
    EXPECT_TRUE(vfs.exists("game://data/small.bin"));
    EXPECT_EQ(*vfs.readText("game://root.txt"), "hello");
    EXPECT_EQ(vfs.list("game://data").size(), 2u);
    EXPECT_FALSE(vfs.exists("game://nope"));
    EXPECT_TRUE((*reader)->verify().empty());

    // Flip one payload byte of an uncompressed entry: CRC catches it.
    {
        auto raw = *serial::readFileBytes(pak);
        raw[(*reader)->find("root.txt")->offset] ^= std::byte{0x20};
        writeBytes(dir / "corrupt.oxpak", raw);
    }
    auto corrupt = PakReader::open(dir / "corrupt.oxpak");
    ASSERT_TRUE(corrupt);
    EXPECT_FALSE((*corrupt)->read("root.txt"));
    EXPECT_EQ((*corrupt)->verify().size(), 1u);
    // Corrupt TOC => refused at open.
    {
        auto raw = *serial::readFileBytes(pak);
        raw[raw.size() - 3] ^= std::byte{0x01};
        writeBytes(dir / "badtoc.oxpak", raw);
    }
    EXPECT_FALSE(PakReader::open(dir / "badtoc.oxpak"));
}

TEST(Pak, CookProjectAndLoadFromPak) {
    TempDir dir;
    writeTestGltf(dir / "Assets/Models", "box");
    writePng(dir / "Assets/unused.png", gradientImage(8, 8));
    writeText(dir / "Assets/Scripts/game.lua", "return 42");
    AssetRegistry reg(dir.path());
    reg.scan();
    const Uuid model = *reg.uuidForPath("Models/box.gltf");
    const Uuid script = *reg.uuidForPath("Scripts/game.lua");
    // A scene referencing the model's prefab (asset UUIDs inside components).
    registerSceneTypes();
    {
        World world;
        Entity e = world.create("Thing");
        auto& mr = e.add<MeshRendererComponent>();
        ASSERT_TRUE(reg.import(model));
        mr.mesh = *reg.uuidForPath("Models/box.gltf#Mesh/0");
        mr.materials = {*reg.uuidForPath("Models/box.gltf#Material/0")};
        ASSERT_TRUE(saveScene(world, dir / "Assets/Levels/start.oxscene"));
    }
    writeText(dir / "pack.json", R"({"startupScenes": ["Levels/start.oxscene"], "alwaysInclude": ["Scripts/"]})");
    const fs::path pak = dir / "Game.oxpak";
    auto report = cookProject(reg, pak);
    ASSERT_TRUE(report) << report.error().message;
    EXPECT_TRUE(report->errors.empty()) << report->errors.front();
    std::set<std::string> paths;
    for (const auto& i : report->items) paths.insert(i.path);
    EXPECT_TRUE(paths.count("Levels/start.oxscene"));
    EXPECT_TRUE(paths.count("Models/box.gltf#Mesh/0"));
    EXPECT_TRUE(paths.count("Models/box.gltf#Material/0"));
    EXPECT_TRUE(paths.count("Models/box_albedo.png"));
    EXPECT_TRUE(paths.count("Models/box_n.png"));
    EXPECT_TRUE(paths.count("Scripts/game.lua"));
    EXPECT_FALSE(paths.count("unused.png"));
    EXPECT_GT(report->pakSize, 0u);

    // Cooked runtime: no registry, no importers.
    PakAssetSource source;
    ASSERT_TRUE(source.addPak(pak));
    JobSystem jobs(2);
    AssetManager mgr(source, &jobs);
    auto scene = mgr.load<SceneAsset>("Levels/start.oxscene");
    auto mat = mgr.load<MaterialAsset>("Models/box.gltf#Material/0");
    auto lua = mgr.load<ScriptAsset>(script);
    mgr.waitAll();
    ASSERT_TRUE(scene.isLoaded()) << scene.error();
    EXPECT_EQ(scene->document.kind, "scene");
    ASSERT_TRUE(mat.isLoaded()) << mat.error();
    EXPECT_TRUE(mgr.find(mat->albedoTexture).isLoaded()); // dependency loaded from the pak
    ASSERT_TRUE(lua.isLoaded());
    EXPECT_EQ(lua->source, "return 42");
    // Texture mips by range from the pak (uncompressed entry).
    auto header = source.readArtifactRange(mat->albedoTexture, 0, kTextureMinHeaderRead);
    ASSERT_TRUE(header);
    auto info = readTextureInfo(*header);
    ASSERT_TRUE(info) << info.error().message;
    RangeReader rr = [&](u64 off, u64 size) { return source.readArtifactRange(mat->albedoTexture, off, size); };
    auto mips = readMipRange(rr, *info, info->desc.mipCount - 2, info->desc.mipCount - 1);
    ASSERT_TRUE(mips);
    EXPECT_EQ(mips->back().width, 1u);
    // The same pak in the VFS.
    Vfs vfs;
    vfs.mount("game", std::make_unique<PakMountSource>(*PakReader::open(pak)));
    EXPECT_TRUE(vfs.exists(std::string("game://") + std::string(kPakCatalogPath)));
}

TEST(Pak, LooseFilesAndPatchPaks) {
    TempDir dir;
    writeText(dir / "Assets/Scripts/game.lua", "return 1");
    writeText(dir / "Assets/UI/menu.rml", "<rml/>");
    writeText(dir / "Assets/UI/style.RCSS", "body {}");
    writeText(dir / "Assets/Data/table.json", R"({"v": 1})");
    writeText(dir / "Assets/Data/notes.txt", "not packed");
    AssetRegistry reg(dir.path());
    reg.scan();
    const Uuid script = *reg.uuidForPath("Scripts/game.lua");
    auto base = cookProject(reg, dir / "Base.oxpak");
    ASSERT_TRUE(base) << base.error().message;
    EXPECT_EQ(base->looseFiles, (std::vector<std::string>{"Assets/Data/table.json", "Assets/UI/menu.rml",
                                                         "Assets/UI/style.RCSS"}));
    auto baseReader = PakReader::open(dir / "Base.oxpak");
    ASSERT_TRUE(baseReader);
    Vfs vfs;
    vfs.mount("project", std::make_unique<PakMountSource>(*baseReader));
    EXPECT_EQ(*vfs.readText("project://Assets/UI/menu.rml"), "<rml/>");
    EXPECT_FALSE(vfs.exists("project://Assets/Data/notes.txt"));

    // Patch: one script and one data file change, everything else stays in the base pak.
    writeText(dir / "Assets/Scripts/game.lua", "return 2");
    writeText(dir / "Assets/Data/table.json", R"({"v": 2})");
    reg.scan();
    CookOptions patchOptions;
    patchOptions.patchBase = {dir / "Base.oxpak"};
    auto patch = cookProject(reg, dir / "Patch.oxpak", patchOptions);
    ASSERT_TRUE(patch) << patch.error().message;
    EXPECT_GT(patch->unchangedSkipped, 0u);
    EXPECT_EQ(patch->looseFiles, std::vector<std::string>{"Assets/Data/table.json"});
    ASSERT_EQ(patch->items.size(), 1u);
    EXPECT_EQ(patch->items[0].path, "Scripts/game.lua");

    PakAssetSource source;
    ASSERT_TRUE(source.addPak(dir / "Base.oxpak"));
    ASSERT_TRUE(source.addPak(dir / "Patch.oxpak"));
    AssetManager mgr(source, nullptr);
    auto lua = mgr.load<ScriptAsset>(script);
    mgr.waitAll();
    ASSERT_TRUE(lua.isLoaded());
    EXPECT_EQ(lua->source, "return 2");
    vfs.mount("project", std::make_unique<PakMountSource>(*PakReader::open(dir / "Patch.oxpak")), 1);
    EXPECT_EQ(*vfs.readText("project://Assets/Data/table.json"), R"({"v": 2})");
    EXPECT_EQ(*vfs.readText("project://Assets/UI/menu.rml"), "<rml/>") << "unchanged files come from the base pak";
}
