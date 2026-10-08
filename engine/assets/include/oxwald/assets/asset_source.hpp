#pragma once

#include <oxwald/assets/asset_types.hpp>
#include <oxwald/core/events.hpp>
#include <oxwald/core/result.hpp>

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ox::assets {

struct AssetRecord {
    Uuid uuid;
    AssetType type = AssetType::Unknown;
    std::string path;              // source path relative to Assets/ ("Textures/rock.png"), sub-assets "model.glb#Mesh0"
    std::vector<Uuid> dependencies; // assets that must be loaded with it (material -> textures)
    u64 artifactSize = 0;
};

// Where an AssetManager gets cooked data from: the editor's AssetRegistry (imports on demand, .oxcache) or
// PakAssetSource (cooked game, .oxpak only). Implementations must be thread-safe.
class IAssetSource {
public:
    virtual ~IAssetSource() = default;

    [[nodiscard]] virtual std::optional<AssetRecord> record(const Uuid& uuid) = 0;
    // Path relative to Assets/ (as in AssetRecord::path).
    [[nodiscard]] virtual std::optional<Uuid> uuidForPath(std::string_view path) = 0;
    [[nodiscard]] virtual Result<std::vector<std::byte>> readArtifact(const Uuid& uuid) = 0;
    // Partial read (texture mip streaming). Default: full read + slice.
    [[nodiscard]] virtual Result<std::vector<std::byte>> readArtifactRange(const Uuid& uuid, u64 offset, u64 size);
    [[nodiscard]] virtual std::vector<Uuid> allAssets() = 0;
    // Called (on the thread that detected it) after an asset's cooked data changed — hot reload.
    virtual Connection subscribeChanges(std::function<void(const Uuid&)> fn) { return {}; }
};

} // namespace ox::assets
