#pragma once

#include <oxwald/assets/importer.hpp>

namespace ox::assets {

// glTF/GLB (fastgltf) and assimp formats -> Prefab (main) + Mesh/Material/Texture/Skeleton/AnimationClip sub-assets.
class ModelImporter final : public IAssetImporter {
public:
    std::string_view name() const override { return "model"; }
    u32 version() const override { return 1; }
    std::vector<std::string> extensions() const override;
    AssetType mainType() const override { return AssetType::Prefab; }
    nlohmann::ordered_json defaultSettings() const override;
    Status import(ImportContext& ctx) override;
};

} // namespace ox::assets
