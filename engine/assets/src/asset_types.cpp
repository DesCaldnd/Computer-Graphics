#include <oxwald/assets/asset_types.hpp>
#include <oxwald/assets/material.hpp>
#include <oxwald/assets/model_import.hpp>
#include <oxwald/assets/texture_import.hpp>

#include "internal.hpp"

#include <oxwald/core/reflect.hpp>

#include <array>
#include <mutex>

namespace ox::assets {

namespace {
struct TypeName {
    AssetType type;
    std::string_view name;
    std::string_view ext;
};
constexpr std::array kTypeNames{
    TypeName{AssetType::Unknown, "Unknown", ".bin"},     TypeName{AssetType::Mesh, "Mesh", ".oxmesh"},
    TypeName{AssetType::Texture, "Texture", ".oxtex"},   TypeName{AssetType::Material, "Material", ".oxmat"},
    TypeName{AssetType::Scene, "Scene", ".oxscene"},     TypeName{AssetType::Prefab, "Prefab", ".oxprefab"},
    TypeName{AssetType::Script, "Script", ".lua"},       TypeName{AssetType::Audio, "Audio", ".oxaudio"},
    TypeName{AssetType::AnimationClip, "AnimationClip", ".oxanim"},
    TypeName{AssetType::Skeleton, "Skeleton", ".oxskel"}, TypeName{AssetType::Font, "Font", ".oxfont"},
    TypeName{AssetType::NavMesh, "NavMesh", ".oxnav"},   TypeName{AssetType::Heightmap, "Heightmap", ".oxheight"},
    TypeName{AssetType::Raw, "Raw", ".bin"},
};
} // namespace

std::string_view assetTypeName(AssetType type) {
    for (const auto& t : kTypeNames) {
        if (t.type == type) return t.name;
    }
    return "Custom";
}

std::optional<AssetType> assetTypeFromName(std::string_view name) {
    for (const auto& t : kTypeNames) {
        if (t.name == name) return t.type;
    }
    return std::nullopt;
}

std::string_view artifactExtension(AssetType type) {
    for (const auto& t : kTypeNames) {
        if (t.type == type) return t.ext;
    }
    return ".bin";
}

namespace builtin {
Uuid checkerTexture() { return Uuid::fromName("ox.builtin.texture.checker"); }
Uuid whiteTexture() { return Uuid::fromName("ox.builtin.texture.white"); }
Uuid flatNormalTexture() { return Uuid::fromName("ox.builtin.texture.flat_normal"); }
Uuid cubeMesh() { return Uuid::fromName("ox.builtin.mesh.cube"); }
Uuid defaultMaterial() { return Uuid::fromName("ox.builtin.material.default"); }
} // namespace builtin

namespace {
void registerAssetTypesOnce();
} // namespace

// Called from job threads too (loadMaterial, importers): re-registering replaces the field lists of the types, which
// races with concurrent readers/registrations, so the (constant) registration runs exactly once per process.
void registerAssetTypes() {
    static std::once_flag once;
    std::call_once(once, registerAssetTypesOnce);
}

namespace {
void registerAssetTypesOnce() {
    using namespace ox::attr;
    OX_REFLECT_ENUM(AssetType, "AssetType")
        .value("Unknown", AssetType::Unknown)
        .value("Mesh", AssetType::Mesh)
        .value("Texture", AssetType::Texture)
        .value("Material", AssetType::Material)
        .value("Scene", AssetType::Scene)
        .value("Prefab", AssetType::Prefab)
        .value("Script", AssetType::Script)
        .value("Audio", AssetType::Audio)
        .value("AnimationClip", AssetType::AnimationClip)
        .value("Skeleton", AssetType::Skeleton)
        .value("Font", AssetType::Font)
        .value("NavMesh", AssetType::NavMesh)
        .value("Heightmap", AssetType::Heightmap)
        .value("Raw", AssetType::Raw);

    OX_REFLECT_ENUM(ShadingModel, "ShadingModel")
        .value("Lit", ShadingModel::Lit)
        .value("Unlit", ShadingModel::Unlit)
        .value("Subsurface", ShadingModel::Subsurface)
        .value("Foliage", ShadingModel::Foliage);
    OX_REFLECT_ENUM(BlendMode, "BlendMode")
        .value("Opaque", BlendMode::Opaque)
        .value("AlphaTest", BlendMode::AlphaTest)
        .value("Transparent", BlendMode::Transparent)
        .value("Refractive", BlendMode::Refractive);

    OX_REFLECT_TYPE(MaterialAsset, "Material")
        .field("shadingModel", &MaterialAsset::shadingModel)
        .field("blendMode", &MaterialAsset::blendMode)
        .field("baseColor", &MaterialAsset::baseColor, Color{false}, Category{"Surface"})
        .field("metallic", &MaterialAsset::metallic, Range{0.0, 1.0}, Category{"Surface"})
        .field("roughness", &MaterialAsset::roughness, Range{0.0, 1.0}, Category{"Surface"})
        .field("emissive", &MaterialAsset::emissive, Color{true}, Category{"Emission"})
        .field("emissiveStrength", &MaterialAsset::emissiveStrength, Range{0.0, 100000.0}, Category{"Emission"})
        .field("normalStrength", &MaterialAsset::normalStrength, Range{0.0, 4.0}, Category{"Surface"})
        .field("occlusionStrength", &MaterialAsset::occlusionStrength, Range{0.0, 1.0}, Category{"Surface"})
        .field("heightScale", &MaterialAsset::heightScale, Range{0.0, 0.2}, Category{"Surface"})
        .field("albedoTexture", &MaterialAsset::albedoTexture, AssetRef{"Texture"}, Category{"Textures"})
        .field("normalTexture", &MaterialAsset::normalTexture, AssetRef{"Texture"}, Category{"Textures"})
        .field("ormTexture", &MaterialAsset::ormTexture, AssetRef{"Texture"}, Category{"Textures"},
               Tooltip{"R = occlusion, G = roughness, B = metallic"})
        .field("emissiveTexture", &MaterialAsset::emissiveTexture, AssetRef{"Texture"}, Category{"Textures"})
        .field("heightTexture", &MaterialAsset::heightTexture, AssetRef{"Texture"}, Category{"Textures"})
        .field("alphaCutoff", &MaterialAsset::alphaCutoff, Range{0.0, 1.0})
        .field("doubleSided", &MaterialAsset::doubleSided)
        .field("ior", &MaterialAsset::ior, DisplayName{"IOR"}, Range{1.0, 3.0}, Category{"Transmission"})
        .field("transmission", &MaterialAsset::transmission, Range{0.0, 1.0}, Category{"Transmission"})
        .field("thickness", &MaterialAsset::thickness, Range{0.0, 10.0}, Category{"Transmission"})
        .field("absorptionColor", &MaterialAsset::absorptionColor, Color{false}, Category{"Transmission"})
        .field("absorptionDistance", &MaterialAsset::absorptionDistance, Range{0.0, 1000.0},
               Category{"Transmission"}, Tooltip{"Beer-Lambert distance at which absorptionColor is reached"})
        .field("clearcoat", &MaterialAsset::clearcoat, Range{0.0, 1.0}, Category{"Clearcoat"})
        .field("clearcoatRoughness", &MaterialAsset::clearcoatRoughness, Range{0.0, 1.0}, Category{"Clearcoat"})
        .field("subsurface", &MaterialAsset::subsurface, Range{0.0, 1.0}, Category{"Subsurface"})
        .field("subsurfaceColor", &MaterialAsset::subsurfaceColor, Color{false}, Category{"Subsurface"})
        .field("uvTiling", &MaterialAsset::uvTiling, Category{"UV"})
        .field("uvOffset", &MaterialAsset::uvOffset, Category{"UV"})
        .field("renderQueueOffset", &MaterialAsset::renderQueueOffset, Range{-1000.0, 1000.0});

    OX_REFLECT_ENUM(TextureType, "TextureType")
        .value("Color", TextureType::Color)
        .value("Normal", TextureType::Normal)
        .value("Linear", TextureType::Linear)
        .value("HDR", TextureType::HDR)
        .value("UI", TextureType::UI);
    OX_REFLECT_ENUM(TextureCompression, "TextureCompression")
        .value("Default", TextureCompression::Default)
        .value("Uncompressed", TextureCompression::Uncompressed);
    OX_REFLECT_ENUM(CubemapMode, "CubemapMode")
        .value("None", CubemapMode::None)
        .value("FromEquirect", CubemapMode::FromEquirect);
    OX_REFLECT_ENUM(MipFilter, "MipFilter").value("Box", MipFilter::Box).value("Kaiser", MipFilter::Kaiser);
    OX_REFLECT_ENUM(TextureWrap, "TextureWrap")
        .value("Repeat", TextureWrap::Repeat)
        .value("Clamp", TextureWrap::Clamp)
        .value("Mirror", TextureWrap::Mirror);
    OX_REFLECT_ENUM(TextureFilter, "TextureFilter")
        .value("Linear", TextureFilter::Linear)
        .value("Nearest", TextureFilter::Nearest);

    OX_REFLECT_TYPE(TextureImportSettings, "TextureImportSettings")
        .field("type", &TextureImportSettings::type)
        .field("srgb", &TextureImportSettings::srgb, DisplayName{"sRGB"})
        .field("maxSize", &TextureImportSettings::maxSize, Tooltip{"0 = unlimited"})
        .field("compression", &TextureImportSettings::compression)
        .field("generateMips", &TextureImportSettings::generateMips)
        .field("mipFilter", &TextureImportSettings::mipFilter)
        .field("preserveAlphaCoverage", &TextureImportSettings::preserveAlphaCoverage)
        .field("alphaCutoff", &TextureImportSettings::alphaCutoff, Range{0.0, 1.0})
        .field("flipY", &TextureImportSettings::flipY)
        .field("wrapU", &TextureImportSettings::wrapU)
        .field("wrapV", &TextureImportSettings::wrapV)
        .field("filter", &TextureImportSettings::filter)
        .field("streamingPriority", &TextureImportSettings::streamingPriority, Range{0.0, 255.0})
        .field("cubemap", &TextureImportSettings::cubemap)
        .field("cubeFaceSize", &TextureImportSettings::cubeFaceSize)
        .field("compressionQuality", &TextureImportSettings::compressionQuality, Range{0.0, 4.0});

    OX_REFLECT_ENUM(UpAxis, "UpAxis")
        .value("Auto", UpAxis::Auto)
        .value("Y", UpAxis::Y)
        .value("Z", UpAxis::Z)
        .value("X", UpAxis::X);
    OX_REFLECT_TYPE(ModelImportSettings, "ModelImportSettings")
        .field("scale", &ModelImportSettings::scale)
        .field("unitToMeters", &ModelImportSettings::unitToMeters, Tooltip{"0 = auto"})
        .field("upAxis", &ModelImportSettings::upAxis)
        .field("mergeMeshes", &ModelImportSettings::mergeMeshes)
        .field("createPrefab", &ModelImportSettings::createPrefab)
        .field("importMaterials", &ModelImportSettings::importMaterials)
        .field("importAnimations", &ModelImportSettings::importAnimations)
        .field("generateTangents", &ModelImportSettings::generateTangents)
        .field("flipUVs", &ModelImportSettings::flipUVs)
        .field("weldVertices", &ModelImportSettings::weldVertices)
        .field("optimize", &ModelImportSettings::optimize)
        .field("generateLods", &ModelImportSettings::generateLods)
        .field("lodRatios", &ModelImportSettings::lodRatios)
        .field("lodMaxError", &ModelImportSettings::lodMaxError)
        .field("generateMeshlets", &ModelImportSettings::generateMeshlets)
        .field("generateCollision", &ModelImportSettings::generateCollision)
        .field("collisionSimplifyRatio", &ModelImportSettings::collisionSimplifyRatio)
        .field("maxHullVertices", &ModelImportSettings::maxHullVertices);

    detail::registerImporterSettingsTypes();
}
} // namespace

} // namespace ox::assets
