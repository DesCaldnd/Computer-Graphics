#pragma once

// Runtime types of the non-mesh/texture/material assets.

#include <oxwald/assets/asset_types.hpp>
#include <oxwald/core/result.hpp>
#include <oxwald/core/serial/value.hpp>

#include <nlohmann/json.hpp>

#include <span>
#include <string>
#include <vector>

#if OX_ASSETS_HAS_ANIMATION
#include <oxwald/animation/clip.hpp>
#include <oxwald/animation/skeleton.hpp>
#endif

namespace ox::assets {

struct SceneAsset {
    static constexpr AssetType kAssetType = AssetType::Scene;
    serial::Document document; // kind "scene" — ox::deserializeWorld(world, document)
    [[nodiscard]] usize memoryUsage() const;
};

struct PrefabAsset {
    static constexpr AssetType kAssetType = AssetType::Prefab;
    serial::Document document; // kind "prefab" — ox::instantiatePrefab(world, document)
    [[nodiscard]] usize memoryUsage() const;
};

struct ScriptAsset {
    static constexpr AssetType kAssetType = AssetType::Script;
    std::string path; // relative to Assets/ (chunk name for Lua)
    std::string source;
    [[nodiscard]] usize memoryUsage() const { return source.size() + path.size(); }
};

// Opaque payload + metadata ("OXBL" artifact). Audio info: format, sampleRate, channels, frames, duration.
// Heightmap info: width, height, format ("r16", "r32f", "r8"). Font/NavMesh: size only.
struct BlobAsset {
    AssetType type = AssetType::Raw;
    nlohmann::ordered_json info = nlohmann::ordered_json::object();
    std::vector<std::byte> data;
    [[nodiscard]] usize memoryUsage() const { return data.size(); }
};
struct AudioClipAsset : BlobAsset {
    static constexpr AssetType kAssetType = AssetType::Audio;
};
struct FontAsset : BlobAsset {
    static constexpr AssetType kAssetType = AssetType::Font;
};
struct NavMeshAsset : BlobAsset {
    static constexpr AssetType kAssetType = AssetType::NavMesh;
};
struct HeightmapAsset : BlobAsset {
    static constexpr AssetType kAssetType = AssetType::Heightmap;
};
struct RawAsset : BlobAsset {
    static constexpr AssetType kAssetType = AssetType::Raw;
};

[[nodiscard]] std::vector<std::byte> serializeBlob(AssetType type, const nlohmann::ordered_json& info,
                                                   std::span<const std::byte> data);
[[nodiscard]] Result<BlobAsset> deserializeBlob(std::span<const std::byte> data);

#if OX_ASSETS_HAS_ANIMATION
struct SkeletonAsset {
    static constexpr AssetType kAssetType = AssetType::Skeleton;
    anim::Skeleton skeleton;
    [[nodiscard]] usize memoryUsage() const;
};
struct AnimationClipAsset {
    static constexpr AssetType kAssetType = AssetType::AnimationClip;
    anim::AnimationClip clip;
    Uuid skeleton;
    [[nodiscard]] usize memoryUsage() const;
};
// "OXSK"/"OXAN" wrappers around ox::anim::serialize.
[[nodiscard]] std::vector<std::byte> serializeSkeletonAsset(const anim::Skeleton& skeleton);
[[nodiscard]] Result<SkeletonAsset> deserializeSkeletonAsset(std::span<const std::byte> data);
[[nodiscard]] std::vector<std::byte> serializeClipAsset(const anim::AnimationClip& clip, const Uuid& skeleton);
[[nodiscard]] Result<AnimationClipAsset> deserializeClipAsset(std::span<const std::byte> data);
#endif

// Every Uuid-tagged value in a document (asset references in scenes/prefabs; entity ids are EntityRef/Id
// components and are filtered by the caller against known assets).
[[nodiscard]] std::vector<Uuid> collectUuidRefs(const serial::Value& value);

} // namespace ox::assets
