// Глава 31: сборка игры в .oxpak (cookProject = tools/oxpack) и загрузка из пака (docs/guide/31-assets.md).
#include "project_fixture.hpp"

#include <oxwald/assets/assets.hpp>
#include <oxwald/core/jobs.hpp>
#include <oxwald/core/vfs.hpp>
#include <oxwald/scene/scene.hpp>
#include <oxwald/scene/scene_serializer.hpp>

#include <gtest/gtest.h>

#include <set>

using namespace ox;
using namespace ox::assets;
using namespace guide31;

namespace {

// Уровень с одной сущностью, которая рисует меш модели «холм» её материалом.
void writeLevel(AssetRegistry& registry, const std::filesystem::path& path) {
    registerSceneTypes();
    ASSERT_TRUE(registry.import(*registry.uuidForPath("Models/hill.obj")));
    World world;
    Entity hill = world.create("Hill");
    auto& mr = hill.add<MeshRendererComponent>();
    mr.mesh = *registry.uuidForPath("Models/hill.obj#Mesh/0");
    mr.materials = {*registry.uuidForPath("Models/hill.obj#Material/0")};
    ASSERT_TRUE(saveScene(world, path));
}

} // namespace

TEST(GuideAssetsPak, CookOnlyWhatTheGameUses) {
    TempProject project;
    writeHillObj(project.asset("Models"));
    writeGradientPng(project.asset("Unused/old_concept.png"), 16, 16);   // нигде не используется
    writeText(project.asset("UI/menu.rml"), "<rml/>");                    // без импортёра — в пак не попадёт
    writeText(project.asset("Scripts/game.lua"), "return 42\n");
    AssetRegistry registry(project.root());
    registry.scan();
    writeLevel(registry, project.asset("Levels/start.oxscene"));

    // pack.json рядом с Assets/: стартовые сцены + то, что нужно всегда (скрипты грузятся по имени).
    writeText(project.root() / "pack.json", R"({"startupScenes": ["Levels/start.oxscene"], "alwaysInclude": ["Scripts/"]})");
    registry.scan();

    const auto pakPath = project.root() / "Build/Game.oxpak";
    std::filesystem::create_directories(pakPath.parent_path());
    CookOptions options;   // = oxpack без флагов; compress = true, compressionLevel = 6, alignment = 16
    auto report = cookProject(registry, pakPath, options);
    ASSERT_TRUE(report) << report.error().message;
    EXPECT_TRUE(report->errors.empty());

    std::set<std::string> paths;
    for (const auto& item : report->items) paths.insert(item.path);
    EXPECT_TRUE(paths.count("Levels/start.oxscene"));
    EXPECT_TRUE(paths.count("Models/hill.obj#Mesh/0"));       // зависимости сцены — транзитивно
    EXPECT_TRUE(paths.count("Models/hill.obj#Material/0"));
    EXPECT_TRUE(paths.count("Models/hill_albedo.png"));
    EXPECT_TRUE(paths.count("Scripts/game.lua"));             // alwaysInclude
    EXPECT_FALSE(paths.count("Unused/old_concept.png"));      // недостижимое не пакуется
    EXPECT_GT(report->pakSize, 0u);

    // Целостность (= oxpack --verify): CRC каждой записи.
    auto reader = PakReader::open(pakPath);
    ASSERT_TRUE(reader) << reader.error().message;
    EXPECT_TRUE((*reader)->verify().empty());
    EXPECT_NE((*reader)->find(kPakCatalogPath), nullptr);   // catalog.oxcat: UUID → тип, путь, зависимости
}

TEST(GuideAssetsPak, CookedGameLoadsFromPakOnly) {
    TempProject project;
    writeHillObj(project.asset("Models"));
    const auto pakPath = project.root() / "Game.oxpak";
    {
        AssetRegistry registry(project.root());
        registry.scan();
        writeLevel(registry, project.asset("Levels/start.oxscene"));
        registry.scan();
        auto report = cookProject(registry, pakPath, {.startupScenes = {"Levels/start.oxscene"}});
        ASSERT_TRUE(report);
    }
    std::filesystem::remove_all(project.root() / "Assets");   // в релиз уходит только пак
    std::filesystem::remove_all(project.root() / ".oxcache");

    // Собранная игра: никаких импортёров, только PakAssetSource.
    PakAssetSource source;
    ASSERT_TRUE(source.addPak(pakPath));   // следующие addPak перекрывают предыдущие — патчи
    JobSystem jobs(2);
    AssetManager assets(source, &jobs);
    auto scene = assets.load<SceneAsset>("Levels/start.oxscene");   // пути те же, что в редакторе
    auto material = assets.load<MaterialAsset>("Models/hill.obj#Material/0");
    assets.waitAll();
    ASSERT_TRUE(scene.isLoaded()) << scene.error();
    ASSERT_TRUE(material.isLoaded()) << material.error();
    World world;
    ASSERT_TRUE(deserializeWorld(world, scene->document));
    EXPECT_TRUE(world.findByName("Hill").valid());

    // Мипы текстуры читаются диапазонами (текстуры в паке не сжимаются zstd) — основа стриминга.
    const Uuid albedo = material->albedoTexture;
    auto header = source.readArtifactRange(albedo, 0, kTextureMinHeaderRead);
    ASSERT_TRUE(header);
    auto info = readTextureInfo(*header);
    ASSERT_TRUE(info) << info.error().message;
    RangeReader reader = [&](u64 offset, u64 size) { return source.readArtifactRange(albedo, offset, size); };
    auto tail = readMipRange(reader, *info, info->desc.mipCount - 2, info->desc.mipCount - 1);   // 2×2 и 1×1
    ASSERT_TRUE(tail);
    EXPECT_EQ(tail->back().width, 1u);

    // Сырые файлы пака доступны через VFS.
    Vfs vfs;
    vfs.mount("game", std::make_unique<PakMountSource>(*PakReader::open(pakPath)));
    EXPECT_TRUE(vfs.exists("game://catalog.oxcat"));
}
