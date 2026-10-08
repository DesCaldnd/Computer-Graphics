#include "test_helpers.hpp"

using namespace oxtest;

namespace {
MaterialAsset sampleMaterial() {
    MaterialAsset m;
    m.shadingModel = ShadingModel::Foliage;
    m.blendMode = BlendMode::Refractive;
    m.baseColor = {0.1f, 0.2f, 0.3f, 0.4f};
    m.metallic = 0.25f;
    m.roughness = 0.125f;
    m.emissive = {1, 2, 3};
    m.emissiveStrength = 10.0f;
    m.albedoTexture = Uuid::fromName("albedo");
    m.normalTexture = Uuid::fromName("normal");
    m.ior = 1.33f;
    m.transmission = 1.0f;
    m.absorptionColor = {0.2f, 0.5f, 0.9f};
    m.absorptionDistance = 2.5f;
    m.uvTiling = {4, 2};
    m.renderQueueOffset = -3;
    return m;
}
} // namespace

TEST(Material, JsonRoundTrip) {
    const MaterialAsset m = sampleMaterial();
    const std::string json = materialToJson(m);
    EXPECT_NE(json.find("\"shadingModel\": \"Foliage\""), std::string::npos);
    EXPECT_NE(json.find(Uuid::fromName("albedo").toString()), std::string::npos);
    auto back = loadMaterial(std::span(reinterpret_cast<const std::byte*>(json.data()), json.size()));
    ASSERT_TRUE(back) << back.error().message;
    EXPECT_EQ(*back, m);
}

TEST(Material, BinaryRoundTripAndDependencies) {
    const MaterialAsset m = sampleMaterial();
    auto back = loadMaterial(materialToBinary(m));
    ASSERT_TRUE(back);
    EXPECT_EQ(*back, m);
    auto deps = m.textureDependencies();
    EXPECT_EQ(deps.size(), 2u);
}

TEST(Material, HandWrittenPartialJson) {
    const std::string json = R"({"blendMode": "AlphaTest", "baseColor": [1, 0, 0, 1], "alphaCutoff": 0.25,
                                 "unknownField": 5})";
    auto m = loadMaterial(std::span(reinterpret_cast<const std::byte*>(json.data()), json.size()));
    ASSERT_TRUE(m) << m.error().message;
    EXPECT_EQ(m->blendMode, BlendMode::AlphaTest);
    EXPECT_FLOAT_EQ(m->alphaCutoff, 0.25f);
    EXPECT_FLOAT_EQ(m->roughness, 0.5f); // default kept
    EXPECT_EQ(m->baseColor, glm::vec4(1, 0, 0, 1));
}

TEST(Material, OxmatInProject) {
    TempDir dir;
    writePng(dir / "Assets/t.png", gradientImage(8, 8));
    AssetRegistry reg(dir.path());
    reg.scan();
    MaterialAsset m;
    m.albedoTexture = *reg.uuidForPath("t.png");
    ASSERT_TRUE(saveMaterial(m, dir / "Assets/m.oxmat"));
    reg.scan();
    const Uuid mat = *reg.uuidForPath("m.oxmat");
    auto rec = reg.record(mat);
    ASSERT_TRUE(rec);
    EXPECT_EQ(rec->type, AssetType::Material);
    EXPECT_EQ(rec->dependencies, std::vector<Uuid>{m.albedoTexture});
    auto loaded = loadMaterial(*reg.readArtifact(mat));
    ASSERT_TRUE(loaded);
    EXPECT_EQ(*loaded, m);
}
