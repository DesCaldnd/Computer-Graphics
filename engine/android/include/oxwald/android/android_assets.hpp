#pragma once

// APK assets through AAssetManager as an ox::IAssetBundle (see <oxwald/core/asset_bundle.hpp>), plus the
// first-launch extraction of the files that must exist as native paths.
//
// APK layout produced by apps/android (Gradle task packageOxwaldAssets):
//   assets/oxindex.txt             index of every file below (AAssetDir cannot list directories)
//   assets/oxbuild.txt             build stamp; extraction reruns when it changes
//   assets/oxwald/engine/shaders/  engine GLSL sources (runtime compilation, SPIR-V cached in user://cache)
//   assets/oxwald/engine/ui/resources/  UI fonts / RmlUi sample documents
//   assets/project/<Name>.oxpak    cooked project (oxpack)
#include <oxwald/core/asset_bundle.hpp>

#include <filesystem>
#include <optional>
#include <string>

struct AAssetManager;

namespace ox::android {

class AndroidAssetBundle final : public IAssetBundle {
public:
    explicit AndroidAssetBundle(AAssetManager* manager) : m_manager(manager) {}
    [[nodiscard]] std::optional<std::vector<std::byte>> read(std::string_view path) const override;
    [[nodiscard]] std::vector<std::string> listFiles(std::string_view dir) const override;
    [[nodiscard]] AAssetManager* manager() const { return m_manager; }

private:
    AAssetManager* m_manager;
};

struct PreparedContent {
    std::filesystem::path engineRoot;   // engine:// root (contains engine/shaders, engine/ui/resources)
    std::filesystem::path pak;          // extracted cooked project, empty when the APK has none
    std::filesystem::path userDir;      // user:// (settings, saves, caches)
    AssetIndex index;
    bool extracted = false;             // false = up to date from a previous launch
};

// Reads oxindex.txt, extracts "oxwald/" and "project/" to `filesDir` when oxbuild.txt changed since the last
// launch, and points OXWALD_SOURCE_DIR / OXWALD_SHADER_DIR / OXWALD_UI_RESOURCE_DIR at the extracted engine files.
[[nodiscard]] Result<PreparedContent> prepareContent(const AndroidAssetBundle& bundle, const std::filesystem::path& filesDir);

} // namespace ox::android
