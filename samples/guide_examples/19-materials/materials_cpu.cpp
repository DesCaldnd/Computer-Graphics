// Глава 19: формат .oxmat, MaterialAsset в коде, сохранение и загрузка, материалы в проекте (AssetRegistry +
// AssetManager) и назначение на MeshRendererComponent (docs/guide/19-materials.md). Устройство не нужно.
#include <oxwald/assets/assets.hpp>
#include <oxwald/scene/scene.hpp>
#include <oxwald/scene/world.hpp>

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <string>

using namespace ox;
using namespace ox::assets;
namespace fs = std::filesystem;

namespace {

fs::path materialsDir() { return fs::path(OX_GUIDE_DIR) / "materials"; }

MaterialAsset loadOrFail(const std::string& file) {
    Result<MaterialAsset> m = loadMaterialFile(materialsDir() / file);
    EXPECT_TRUE(m) << file << ": " << (m ? std::string() : m.error().message);
    return m ? *m : MaterialAsset{};
}

fs::path tempProject() {
    const fs::path dir = fs::temp_directory_path() / ("oxwald_guide_materials_" + Uuid::generate().toString());
    fs::create_directories(dir / "Assets");
    return dir;
}

} // namespace

TEST(GuideMaterials, LoadHandWrittenOxmat) {
    // Минимальный файл: всё, чего нет в JSON, остаётся по умолчанию.
    const MaterialAsset plastic = loadOrFail("red_plastic.oxmat");
    EXPECT_EQ(plastic.blendMode, BlendMode::Opaque);
    EXPECT_EQ(plastic.shadingModel, ShadingModel::Lit);
    EXPECT_FLOAT_EQ(plastic.roughness, 0.35f);
    EXPECT_FLOAT_EQ(plastic.metallic, 0.0f);
    EXPECT_FLOAT_EQ(plastic.ior, 1.5f);

    const MaterialAsset gold = loadOrFail("brushed_gold.oxmat");
    EXPECT_FLOAT_EQ(gold.metallic, 1.0f);

    const MaterialAsset glass = loadOrFail("green_glass.oxmat");
    EXPECT_EQ(glass.blendMode, BlendMode::Refractive);
    EXPECT_FLOAT_EQ(glass.ior, 1.5f);
    EXPECT_EQ(glass.absorptionColor, glm::vec3(0.45f, 0.9f, 0.55f));
    EXPECT_FLOAT_EQ(glass.absorptionDistance, 1.0f);

    const MaterialAsset window = loadOrFail("tinted_window.oxmat");
    EXPECT_EQ(window.blendMode, BlendMode::Transparent);
    EXPECT_FLOAT_EQ(window.baseColor.a, 0.35f);
    EXPECT_TRUE(window.doubleSided);

    const MaterialAsset neon = loadOrFail("neon_sign.oxmat");
    EXPECT_EQ(neon.emissive, glm::vec3(1.0f, 0.1f, 0.6f));
    EXPECT_FLOAT_EQ(neon.emissiveStrength, 4.0f);

    const MaterialAsset leaves = loadOrFail("leaves.oxmat");
    EXPECT_EQ(leaves.blendMode, BlendMode::AlphaTest);
    EXPECT_EQ(leaves.shadingModel, ShadingModel::Foliage);
    EXPECT_FLOAT_EQ(leaves.alphaCutoff, 0.4f);
    EXPECT_EQ(leaves.uvTiling, glm::vec2(2.0f));
}

TEST(GuideMaterials, CreateInCodeSaveAndLoad) {
    MaterialAsset m;
    m.blendMode = BlendMode::Refractive;
    m.baseColor = {1.0f, 1.0f, 1.0f, 1.0f};
    m.roughness = 0.05f;
    m.ior = 1.33f;                          // вода
    m.absorptionColor = {0.3f, 0.7f, 0.9f}; // цвет после absorptionDistance метров пути в среде
    m.absorptionDistance = 2.0f;
    m.albedoTexture = Uuid::fromName("Textures/caustics.png"); // ссылки на текстуры — UUID ассетов
    m.normalTexture = Uuid::fromName("Textures/ripples_normal.png");

    const std::string json = materialToJson(m); // то же, что пишет saveMaterial(m, "x.oxmat")
    std::printf("%s", json.c_str());
    EXPECT_NE(json.find("\"oxmat\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"blendMode\": \"Refractive\""), std::string::npos);
    EXPECT_NE(json.find(m.albedoTexture.toString()), std::string::npos);

    const fs::path dir = tempProject();
    ASSERT_TRUE(saveMaterial(m, dir / "water.oxmat"));   // JSON (.oxmat/.json)
    ASSERT_TRUE(saveMaterial(m, dir / "water.bin"));     // любое другое расширение — бинарный OXB1
    auto fromJson = loadMaterialFile(dir / "water.oxmat");
    auto fromBinary = loadMaterialFile(dir / "water.bin");
    ASSERT_TRUE(fromJson && fromBinary);
    EXPECT_EQ(*fromJson, m);
    EXPECT_EQ(*fromBinary, m);
    EXPECT_EQ(m.textureDependencies().size(), 2u); // загрузятся раньше материала
    fs::remove_all(dir);
}

TEST(GuideMaterials, TyposKeepDefaults) {
    // Опечатка в значении перечисления — не ошибка загрузки: поле остаётся по умолчанию, в лог уходит
    // предупреждение "[serial] Material.blendMode: cannot convert string value, keeping default".
    const std::string typo = R"({"blendMode": "Glass", "roughness": 0.2})";
    auto r = loadMaterial(std::span(reinterpret_cast<const std::byte*>(typo.data()), typo.size()));
    ASSERT_TRUE(r);
    EXPECT_EQ(r->blendMode, BlendMode::Opaque);
    EXPECT_FLOAT_EQ(r->roughness, 0.2f);
    // Неизвестные ключи (опечатка в имени поля) молча игнорируются.
    const std::string unknownKey = R"({"roughnes": 0.9})";
    auto r2 = loadMaterial(std::span(reinterpret_cast<const std::byte*>(unknownKey.data()), unknownKey.size()));
    ASSERT_TRUE(r2);
    EXPECT_FLOAT_EQ(r2->roughness, 0.5f);
    // Невалидный JSON — ошибка.
    const std::string notJson = "blendMode = Opaque";
    EXPECT_FALSE(loadMaterial(std::span(reinterpret_cast<const std::byte*>(notJson.data()), notJson.size())));
}

TEST(GuideMaterials, OxmatInProjectAssignedToMesh) {
    const fs::path project = tempProject();
    fs::create_directories(project / "Assets/Materials");
    fs::copy_file(materialsDir() / "red_plastic.oxmat", project / "Assets/Materials/red_plastic.oxmat");

    AssetRegistry registry(project); // <project>/Assets, рядом создаются .meta с UUID
    registry.scan();
    const std::optional<Uuid> id = registry.uuidForPath("Materials/red_plastic.oxmat");
    ASSERT_TRUE(id);
    EXPECT_TRUE(fs::exists(project / "Assets/Materials/red_plastic.oxmat.meta"));
    EXPECT_EQ(registry.record(*id)->type, AssetType::Material);

    AssetManager assets(registry);
    AssetHandle<MaterialAsset> mat = assets.loadSync<MaterialAsset>("Materials/red_plastic.oxmat");
    ASSERT_TRUE(mat.isLoaded()) << mat.error();
    EXPECT_FLOAT_EQ(mat->roughness, 0.35f);

    // Назначение: по UUID, по одному материалу на сабмеш (слот) меша.
    registerSceneTypes();
    World world;
    Entity e = world.create("Crate");
    auto& mr = e.add<MeshRendererComponent>();
    mr.mesh = builtin::cubeMesh();
    mr.materials = {*id};
    EXPECT_EQ(e.get<MeshRendererComponent>().materials.front(), *id);
    fs::remove_all(project);
}
