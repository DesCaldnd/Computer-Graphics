#pragma once

#include <oxwald/animation/clip.hpp>
#include <oxwald/animation/skinning.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ox::anim {

enum class UpAxis : u8 { Auto, Y, Z, X };

struct ImportSettings {
    // Auto reads the FBX global settings metadata (UpAxis / UnitScaleFactor); glTF is Y-up meters.
    UpAxis sourceUpAxis = UpAxis::Auto;
    // Multiplies all distances after the unit conversion (e.g. art authored at a different scale).
    f32 scale = 1.0f;
    // Overrides the auto-detected unit → meters factor when > 0 (e.g. 0.01 for centimetre FBX).
    f32 unitToMeters = 0.0f;
    u32 maxInfluences = 4; // 4 or 8
    bool importMeshes = true;
    bool importAnimations = true;
    bool fixQuaternionHemispheres = true;
};

struct AnimationImport {
    Skeleton skeleton;
    std::vector<SkinnedMeshData> meshes;
    std::vector<AnimationClip> clips;
};

// Imports skeleton + skinned meshes + clips from glTF/GLB/FBX/... via assimp, converting to the engine
// convention (right-handed, Y-up, meters). Returns std::nullopt and fills `error` on failure.
std::optional<AnimationImport> importAnimationAsset(const std::filesystem::path& path,
                                                    const ImportSettings& settings = {},
                                                    std::string* error = nullptr);

// Same, from memory (`formatHint` is a file extension such as "gltf" / "glb" / "fbx").
std::optional<AnimationImport> importAnimationAssetFromMemory(const void* data, usize size, const char* formatHint,
                                                              const ImportSettings& settings = {},
                                                              std::string* error = nullptr);

} // namespace ox::anim
