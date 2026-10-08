// Глава 31: база ассетов — AssetRegistry, .meta и UUID, импорт текстур (BC7/BC5, мипы) и моделей (LOD, меш-леты),
// материалы .oxmat, свой импортёр (docs/guide/31-assets.md).
#include "project_fixture.hpp"

#include <oxwald/assets/assets.hpp>

#include <gtest/gtest.h>

#include <algorithm>

using namespace ox;
using namespace ox::assets;
using namespace guide31;

TEST(GuideAssetsDatabase, ScanCreatesMetasWithStableUuids) {
    TempProject project;
    writeGradientPng(project.asset("Textures/rock.png"), 64, 64);
    writeText(project.asset("Scripts/door.lua"), "function onStart(self) end\n");
    writeText(project.asset("notes.txt"), "нет импортёра — файл игнорируется");

    Uuid rock;
    {
        AssetRegistry registry(project.root());   // встроенные импортёры уже зарегистрированы
        const ScanResult scan = registry.scan();  // создаёт недостающие .meta, строит UUID <-> путь
        EXPECT_EQ(scan.found, 2u);
        EXPECT_EQ(scan.metasCreated, 2u);
        rock = *registry.uuidForPath("Textures/rock.png");   // путь относительно Assets/, разделитель '/'

        // Рядом с исходником появился "<файл>.meta": UUID, импортёр и его настройки.
        auto meta = readMeta(project.asset("Textures/rock.png.meta"));
        ASSERT_TRUE(meta);
        EXPECT_EQ(meta->uuid, rock);
        EXPECT_EQ(meta->importer, "texture");
        EXPECT_EQ(meta->settings["type"], "Color");
        EXPECT_FALSE(std::filesystem::exists(project.asset("notes.txt.meta")));
    }
    // Новый экземпляр (перезапуск редактора) читает те же .meta: UUID стабилен.
    AssetRegistry again(project.root());
    again.scan();
    EXPECT_EQ(again.uuidForPath("Textures/rock.png"), rock);
}

TEST(GuideAssetsDatabase, MovingFileWithMetaKeepsUuid) {
    TempProject project;
    writeGradientPng(project.asset("rock.png"), 16, 16);
    AssetRegistry registry(project.root());
    registry.scan();
    const Uuid id = *registry.uuidForPath("rock.png");

    std::string from, to;
    ScopedConnection c = registry.onMoved.connect([&](const Uuid&, const std::string& f, const std::string& t) {
        from = f;
        to = t;
    });
    std::filesystem::create_directories(project.asset("Nature"));
    std::filesystem::rename(project.asset("rock.png"), project.asset("Nature/rock.png"));
    std::filesystem::rename(project.asset("rock.png.meta"), project.asset("Nature/rock.png.meta"));
    EXPECT_EQ(registry.scan().moved, 1u);
    EXPECT_EQ(registry.uuidForPath("Nature/rock.png"), id);   // ссылки в сценах и материалах не ломаются
    EXPECT_EQ(from, "rock.png");
    EXPECT_EQ(to, "Nature/rock.png");
}

TEST(GuideAssetsDatabase, TextureImportFormatsAndMips) {
    TempProject project;
    writeGradientPng(project.asset("Textures/rock.png"), 64, 64);
    writeNormalPng(project.asset("Textures/rock_normal.png"), 64, 64);   // суффикс _normal → тип Normal
    writeGradientPng(project.asset("Textures/rock_orm.png"), 64, 64);    // суффикс _orm → тип Linear
    AssetRegistry registry(project.root());
    registry.scan();
    const ImportStats stats = registry.importAll();   // результаты кэшируются в .oxcache/
    EXPECT_EQ(stats.imported, 3u);
    EXPECT_EQ(stats.failed, 0u);

    auto load = [&](const char* path) {
        auto tex = deserializeTexture(*registry.readArtifact(*registry.uuidForPath(path)));
        EXPECT_TRUE(tex) << path;
        return *tex;
    };
    const TextureData color = load("Textures/rock.png");
    EXPECT_EQ(color.format, TextureFormat::BC7Srgb);   // цвет: BC7 sRGB
    EXPECT_EQ(color.mipCount, 7u);                     // 64, 32, 16, 8, 4, 2, 1
    EXPECT_EQ(load("Textures/rock_normal.png").format, TextureFormat::BC5Unorm);   // нормали: BC5 (XY)
    EXPECT_EQ(load("Textures/rock_orm.png").format, TextureFormat::BC7Unorm);      // маски: BC7 linear

    // Настройки импорта пишутся в .meta и сразу вызывают реимпорт.
    const Uuid rock = *registry.uuidForPath("Textures/rock.png");
    ASSERT_TRUE(registry.setSettings(rock, {{"compression", "Uncompressed"}, {"maxSize", 32}}));
    const TextureData small = load("Textures/rock.png");
    EXPECT_EQ(small.format, TextureFormat::RGBA8Srgb);
    EXPECT_EQ(small.width, 32u);
    EXPECT_EQ(registry.meta(rock)->settings["maxSize"], 32);

    // Второй запуск: кэш актуален, импортировать нечего.
    EXPECT_EQ(registry.importAll().upToDate, 3u);
}

TEST(GuideAssetsDatabase, ModelImportLodsMeshletsAndSubAssets) {
    TempProject project;
    writeHillObj(project.asset("Models"));
    AssetRegistry registry(project.root());
    registry.scan();
    const Uuid model = *registry.uuidForPath("Models/hill.obj");
    ASSERT_TRUE(registry.import(model));

    // Главный ассет модели — префаб; меши и материалы — под-ассеты "<путь>#<имя>".
    EXPECT_EQ(registry.info(model)->type, AssetType::Prefab);
    const auto meshId = registry.uuidForPath("Models/hill.obj#Mesh/0");
    const auto matId = registry.uuidForPath("Models/hill.obj#Material/0");
    ASSERT_TRUE(meshId && matId);
    EXPECT_EQ(*meshId, Uuid::fromName(model.toString() + "/Mesh/0"));   // UUID под-ассета детерминирован

    auto mesh = deserializeMesh(*registry.readArtifact(*meshId));
    ASSERT_TRUE(mesh) << mesh.error().message;
    EXPECT_EQ(mesh->triangleCount(0), 32u * 32u * 2u);   // LOD 0 — полная детализация
    EXPECT_GE(mesh->lodCount(), 2u);                    // упрощённые LOD (lodRatios 0.5 / 0.25 / 0.125)
    EXPECT_LT(mesh->triangleCount(mesh->lodCount() - 1), mesh->triangleCount(0));
    ASSERT_FALSE(mesh->meshlets.empty());
    for (const Meshlet& m : mesh->meshlets) {
        EXPECT_LE(m.vertexCount, 64u);     // лимиты меш-лета
        EXPECT_LE(m.triangleCount, 124u);
    }

    // Материал из MTL ссылается на текстуру — это зависимость в графе ассетов.
    const Uuid albedo = *registry.uuidForPath("Models/hill_albedo.png");
    auto material = loadMaterial(*registry.readArtifact(*matId));
    ASSERT_TRUE(material);
    EXPECT_EQ(material->albedoTexture, albedo);
    const auto deps = registry.dependencies(*matId);
    EXPECT_NE(std::find(deps.begin(), deps.end(), albedo), deps.end());
    const auto users = registry.dependents(albedo);
    EXPECT_NE(std::find(users.begin(), users.end(), *matId), users.end());
    // Полное замыкание: префаб, меш, материал, текстура.
    EXPECT_GE(registry.collectDependencies(std::vector<Uuid>{model}).size(), 4u);
}

TEST(GuideAssetsDatabase, ModelImportSettingsInMeta) {
    TempProject project;
    writeHillObj(project.asset("Models"));
    AssetRegistry registry(project.root());
    registry.scan();
    const Uuid model = *registry.uuidForPath("Models/hill.obj");
    // Коллизия для физики и без LOD — те же ключи, что в ModelImportSettings.
    ASSERT_TRUE(registry.setSettings(model, {{"generateLods", false}, {"generateCollision", true}, {"scale", 2.0}}));
    auto mesh = deserializeMesh(*registry.readArtifact(*registry.uuidForPath("Models/hill.obj#Mesh/0")));
    ASSERT_TRUE(mesh);
    EXPECT_EQ(mesh->lodCount(), 1u);
    EXPECT_FALSE(mesh->collision.hullVertices.empty());   // выпуклая оболочка
    EXPECT_FALSE(mesh->collision.indices.empty());        // упрощённый треугольный меш
    EXPECT_NEAR(mesh->bounds.max.x, 8.0f, 1e-3f);         // 4 м × scale 2
}

TEST(GuideAssetsDatabase, MaterialFileInProject) {
    TempProject project;
    writeGradientPng(project.asset("Textures/rock.png"), 16, 16);
    AssetRegistry registry(project.root());
    registry.scan();
    const Uuid rock = *registry.uuidForPath("Textures/rock.png");

    // .oxmat — обычный JSON; отсутствующие поля получают значения по умолчанию.
    writeText(project.asset("Materials/rock.oxmat"), R"({
  "oxmat": 1,
  "shadingModel": "Lit",
  "baseColor": [0.8, 0.8, 0.8, 1.0],
  "roughness": 0.85,
  "albedoTexture": ")" + rock.toString() + R"("
})");
    registry.scan();
    const Uuid mat = *registry.uuidForPath("Materials/rock.oxmat");
    EXPECT_EQ(registry.record(mat)->type, AssetType::Material);
    auto m = loadMaterial(*registry.readArtifact(mat));
    ASSERT_TRUE(m) << m.error().message;
    EXPECT_FLOAT_EQ(m->roughness, 0.85f);
    EXPECT_FLOAT_EQ(m->metallic, 0.0f);   // по умолчанию
    EXPECT_EQ(m->albedoTexture, rock);
    EXPECT_EQ(registry.dependencies(mat), std::vector<Uuid>{rock});
}

namespace {

// Свой импортёр: ".dialog" (строки "Имя: реплика") → Raw-ассет с числом реплик в info.
class DialogImporter final : public IAssetImporter {
public:
    std::string_view name() const override { return "dialog"; }
    u32 version() const override { return 1; }   // увеличьте — и все .dialog переимпортируются
    std::vector<std::string> extensions() const override { return {".dialog"}; }
    AssetType mainType() const override { return AssetType::Raw; }
    nlohmann::ordered_json defaultSettings() const override { return {{"trim", true}}; }

    Status import(ImportContext& ctx) override {
        auto bytes = ctx.readSource();
        if (!bytes) return bytes.error();
        const std::string text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
        const auto lines = std::count(text.begin(), text.end(), '\n');
        if (lines == 0) ctx.warn("пустой диалог");
        const nlohmann::ordered_json info = {{"lines", lines}, {"trim", ctx.meta().settings.value("trim", true)}};
        ctx.setMain(AssetType::Raw, serializeBlob(AssetType::Raw, info, *bytes));
        return {};
    }
};

} // namespace

TEST(GuideAssetsDatabase, CustomImporter) {
    TempProject project;
    writeText(project.asset("Dialogs/intro.dialog"), "Хозяин: Добро пожаловать!\nГерой: Спасибо.\n");
    AssetRegistry registry(project.root());
    registry.importers().add(std::make_unique<DialogImporter>());   // до scan(): иначе файл без импортёра
    registry.scan();
    const Uuid id = *registry.uuidForPath("Dialogs/intro.dialog");
    EXPECT_EQ(registry.meta(id)->importer, "dialog");

    AssetManager manager(registry);
    auto dialog = manager.loadSync<RawAsset>(id);
    ASSERT_TRUE(dialog.isLoaded()) << dialog.error();
    EXPECT_EQ(dialog->info["lines"], 2);
    EXPECT_EQ(dialog->info["trim"], true);
}

TEST(GuideAssetsDatabase, StandaloneImportLikeOximport) {
    TempProject project;
    writeHillObj(project.root() / "src");
    // То же, что делает tools/oximport: импорт одного файла вне проекта, .meta не создаются.
    registerAssetTypes();
    ImporterRegistry importers;
    importers.addBuiltins();
    auto artifacts = importStandalone(importers, project.root() / "src/hill.obj", {{"generateCollision", true}});
    ASSERT_TRUE(artifacts) << artifacts.error().message;
    auto mesh = std::find_if(artifacts->begin(), artifacts->end(), [](const ImportedArtifact& a) { return a.type == AssetType::Mesh; });
    ASSERT_NE(mesh, artifacts->end());
    EXPECT_EQ(mesh->name, "Mesh/0");
    EXPECT_FALSE(std::filesystem::exists(project.root() / "src/hill.obj.meta"));
}
