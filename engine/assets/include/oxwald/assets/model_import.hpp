#pragma once

// Model import (meshes + materials + node hierarchy) from glTF/GLB (fastgltf, preferred) or anything assimp
// reads (OBJ, FBX, DAE, 3DS, PLY, ...). Output is engine-convention: right-handed, Y-up, meters.

#include <oxwald/assets/material.hpp>
#include <oxwald/assets/mesh.hpp>
#include <oxwald/assets/mesh_processing.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ox::assets {

enum class UpAxis : u8 { Auto, Y, Z, X };

struct ModelImportSettings {
    f32 scale = 1.0f;          // extra uniform scale after unit conversion
    f32 unitToMeters = 0.0f;   // 0 = auto (FBX UnitScaleFactor, glTF/OBJ = 1)
    UpAxis upAxis = UpAxis::Auto; // source up axis; Auto = file metadata (FBX) or Y
    bool mergeMeshes = false;  // bake the whole hierarchy into one mesh (one submesh per material)
    bool createPrefab = true;  // node hierarchy -> prefab (MeshRenderer entities)
    bool importMaterials = true;
    bool importAnimations = true; // skeleton + clips (animation module)
    bool generateTangents = true; // when the source has none
    bool flipUVs = false;
    bool weldVertices = true;
    // processing
    bool optimize = true;
    bool generateLods = true;
    std::vector<f32> lodRatios{0.5f, 0.25f, 0.125f};
    f32 lodMaxError = 0.05f;
    bool generateMeshlets = true;
    bool generateCollision = false;
    f32 collisionSimplifyRatio = 0.25f;
    u32 maxHullVertices = 64;

    [[nodiscard]] MeshProcessSettings processSettings() const;
};

// Texture referenced by a material: an external file (path relative to the model file) or an embedded image.
struct ModelTextureRef {
    std::string path;        // relative to the model's directory, '/' separators
    i32 embedded = -1;       // index into ImportedModel::embeddedImages
    [[nodiscard]] bool valid() const { return !path.empty() || embedded >= 0; }
};

struct ImportedMaterial {
    std::string name;
    MaterialAsset material; // texture UUIDs left nil; see refs
    ModelTextureRef albedo, normal, orm, emissive, occlusion; // occlusion only when not packed into orm
};

struct EmbeddedImage {
    std::string name;
    std::string extension; // "png", "jpg", "ktx2", ...
    std::vector<std::byte> data;
};

struct ModelNode {
    std::string name;
    i32 parent = -1;
    Transform local;
    std::vector<u32> meshes; // indices into ImportedModel::meshes
};

struct ImportedModel {
    std::vector<MeshData> meshes; // material slots reference ImportedModel::materials by index (slot.name = index)
    std::vector<std::vector<u32>> meshMaterials; // per mesh: material index per slot (-1 = default)
    std::vector<ImportedMaterial> materials;
    std::vector<EmbeddedImage> embeddedImages;
    std::vector<ModelNode> nodes; // parents before children
    std::vector<std::string> jointNames; // SkinVertex::joints index this table (bone/joint node names)
    bool hasSkin = false;
    bool hasAnimations = false;
    std::string importer; // "fastgltf" / "assimp"
};

// Imports without post-processing (no LODs/meshlets); tangents/normals are generated when missing.
[[nodiscard]] Result<ImportedModel> importModel(const std::filesystem::path& path,
                                                const ModelImportSettings& settings = {});
[[nodiscard]] Result<ImportedModel> importModelGltf(const std::filesystem::path& path,
                                                    const ModelImportSettings& settings);
[[nodiscard]] Result<ImportedModel> importModelAssimp(const std::filesystem::path& path,
                                                      const ModelImportSettings& settings);

// One-shot convenience: import + merge everything into a single processed mesh.
[[nodiscard]] Result<MeshData> importMeshFile(const std::filesystem::path& path, const ModelImportSettings& settings = {});

// Bakes the node hierarchy into one mesh (transforms applied), one submesh per material. slotMaterials receives
// the ImportedModel material index of every material slot (-1 = default material).
[[nodiscard]] MeshData mergeModel(const ImportedModel& model, std::vector<i32>* slotMaterials = nullptr);

} // namespace ox::assets
