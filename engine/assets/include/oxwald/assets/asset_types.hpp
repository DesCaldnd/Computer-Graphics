#pragma once

#include <oxwald/core/types.hpp>
#include <oxwald/core/uuid.hpp>

#include <optional>
#include <string_view>

namespace ox::assets {

// Values are stored in .oxpak catalogs and import records: append only, never renumber.
enum class AssetType : u16 {
    Unknown = 0,
    Mesh = 1,
    Texture = 2,
    Material = 3,
    Scene = 4,
    Prefab = 5,
    Script = 6,
    Audio = 7,
    AnimationClip = 8,
    Skeleton = 9,
    Font = 10,
    NavMesh = 11,
    Heightmap = 12,
    Raw = 13,
    FirstCustom = 1000, // other modules: AssetType(FirstCustom + n)
};

[[nodiscard]] std::string_view assetTypeName(AssetType type);
[[nodiscard]] std::optional<AssetType> assetTypeFromName(std::string_view name);
// Extension of the cooked artifact (".oxmesh", ".oxtex", ...).
[[nodiscard]] std::string_view artifactExtension(AssetType type);

// Built-in placeholder assets, always resident in an AssetManager.
namespace builtin {
[[nodiscard]] Uuid checkerTexture(); // 64x64 magenta/grey checker, RGBA8 sRGB
[[nodiscard]] Uuid whiteTexture();   // 4x4 white, RGBA8 sRGB
[[nodiscard]] Uuid flatNormalTexture(); // 4x4 (0.5,0.5,1) RGBA8 linear
[[nodiscard]] Uuid cubeMesh();       // unit cube, 24 vertices
[[nodiscard]] Uuid defaultMaterial(); // Lit, opaque, white, roughness 0.5
} // namespace builtin

// Registers reflection for every asset-side type (materials, import settings). Idempotent.
void registerAssetTypes();

} // namespace ox::assets
