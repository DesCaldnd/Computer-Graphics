#pragma once

#include <oxwald/assets/asset_types.hpp>
#include <oxwald/core/math.hpp>
#include <oxwald/core/result.hpp>

#include <filesystem>
#include <span>
#include <vector>

namespace ox::assets {

enum class ShadingModel : u8 { Lit, Unlit, Subsurface, Foliage };
enum class BlendMode : u8 { Opaque, AlphaTest, Transparent, Refractive };

// Reflected, JSON-editable (.oxmat in Assets is both the source and the runtime format; the cache keeps an
// OXB1 binary copy). Texture fields are Texture asset UUIDs (nil = none).
struct MaterialAsset {
    static constexpr AssetType kAssetType = AssetType::Material;

    ShadingModel shadingModel = ShadingModel::Lit;
    BlendMode blendMode = BlendMode::Opaque;
    glm::vec4 baseColor{1.0f}; // linear RGBA
    f32 metallic = 0.0f;
    f32 roughness = 0.5f;
    glm::vec3 emissive{0.0f}; // linear RGB
    f32 emissiveStrength = 1.0f;
    f32 normalStrength = 1.0f;
    f32 occlusionStrength = 1.0f;
    f32 heightScale = 0.0f; // parallax, 0 = off

    Uuid albedoTexture;   // sRGB colour (+ alpha)
    Uuid normalTexture;   // BC5 tangent-space normal
    Uuid ormTexture;      // R = occlusion, G = roughness, B = metallic (glTF layout)
    Uuid emissiveTexture;
    Uuid heightTexture;

    f32 alphaCutoff = 0.5f;
    bool doubleSided = false;
    f32 ior = 1.5f;
    f32 transmission = 0.0f;
    f32 thickness = 0.0f;
    glm::vec3 absorptionColor{1.0f}; // Beer–Lambert: colour reached after absorptionDistance
    f32 absorptionDistance = 0.0f;   // 0 = no absorption
    f32 clearcoat = 0.0f;
    f32 clearcoatRoughness = 0.0f;
    f32 subsurface = 0.0f;
    glm::vec3 subsurfaceColor{1.0f, 0.3f, 0.2f};
    glm::vec2 uvTiling{1.0f};
    glm::vec2 uvOffset{0.0f};
    i32 renderQueueOffset = 0;

    [[nodiscard]] std::vector<Uuid> textureDependencies() const;
    bool operator==(const MaterialAsset&) const = default;
};

// ".json" or ".oxmat" (JSON) / anything else binary OXB1.
Status saveMaterial(const MaterialAsset& material, const std::filesystem::path& path);
[[nodiscard]] std::string materialToJson(const MaterialAsset& material);
[[nodiscard]] std::vector<std::byte> materialToBinary(const MaterialAsset& material);
// Accepts OXB1 binary or JSON (with or without schema).
[[nodiscard]] Result<MaterialAsset> loadMaterial(std::span<const std::byte> data);
[[nodiscard]] Result<MaterialAsset> loadMaterialFile(const std::filesystem::path& path);

} // namespace ox::assets
