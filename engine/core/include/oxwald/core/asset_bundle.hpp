#pragma once

// Read-only VFS sources for files packaged inside an application bundle: Android APK assets (AAssetManager,
// implemented in engine/android), or anything else that can read a file by its bundle path (tests use an in-memory
// bundle).
//
// AAssetManager can open files but cannot enumerate subdirectories (AAssetDir lists files only), so packaging
// writes an index (`oxindex.txt` at the asset root, one '/'-separated file path per line, '#' comments) that
// BundleMount uses for exists()/list(). Without an index, exists() falls back to reading the file and list() to
// the backend's (non-recursive) listFiles().
//
//   auto bundle = std::make_shared<AndroidAssetBundle>(activity->assetManager);
//   auto index  = AssetIndex::parse(text of bundle->read("oxindex.txt"));
//   vfs.mount("apk", std::make_unique<BundleMount>(bundle, "", index));          // apk://engine/shaders/...
//   vfs.mount("engine", std::make_unique<BundleMount>(bundle, "engine", index)); // engine://shaders/...

#include <oxwald/core/result.hpp>
#include <oxwald/core/vfs.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ox {

class IAssetBundle {
public:
    virtual ~IAssetBundle() = default;
    // `path` is relative to the bundle root, normalised, '/'-separated ("engine/shaders/common/bindless.glsl").
    [[nodiscard]] virtual std::optional<std::vector<std::byte>> read(std::string_view path) const = 0;
    // Files (not directories) directly inside `dir` ("" = root), as names relative to `dir`. Optional.
    [[nodiscard]] virtual std::vector<std::string> listFiles(std::string_view dir) const { return {}; }
};

// Sorted set of file paths in a bundle.
class AssetIndex {
public:
    AssetIndex() = default;
    explicit AssetIndex(std::vector<std::string> files); // normalised, sorted, deduplicated; invalid paths dropped

    // One path per line; blank lines, '#' comments and paths that fail Vfs::normalizePath are skipped. CRLF ok.
    [[nodiscard]] static AssetIndex parse(std::string_view text);
    [[nodiscard]] std::string serialize() const;

    [[nodiscard]] bool contains(std::string_view path) const;
    [[nodiscard]] bool containsDirectory(std::string_view dir) const;
    // Files below `dir` ("" = everything), as full bundle paths.
    [[nodiscard]] std::vector<std::string> filesUnder(std::string_view dir, bool recursive) const;
    [[nodiscard]] const std::vector<std::string>& files() const { return m_files; }
    [[nodiscard]] usize size() const { return m_files.size(); }
    [[nodiscard]] bool empty() const { return m_files.empty(); }

private:
    std::vector<std::string> m_files;
};

// Bundle path of `relPath` below `prefix` ("engine" + "shaders/x.glsl" -> "engine/shaders/x.glsl"), or nullopt
// when either escapes / is absolute (same rules as Vfs::normalizePath).
[[nodiscard]] std::optional<std::string> bundlePath(std::string_view prefix, std::string_view relPath);

// IMountSource over the subtree `prefix` of a bundle (read-only).
class BundleMount final : public IMountSource {
public:
    BundleMount(std::shared_ptr<const IAssetBundle> bundle, std::string prefix, std::optional<AssetIndex> index = {});

    [[nodiscard]] bool exists(std::string_view relPath) const override;
    [[nodiscard]] Result<std::vector<std::byte>> read(std::string_view relPath) const override;
    [[nodiscard]] std::vector<std::string> list(std::string_view relDir, bool recursive) const override;

    [[nodiscard]] const std::string& prefix() const { return m_prefix; }

private:
    std::shared_ptr<const IAssetBundle> m_bundle;
    std::string m_prefix;
    std::optional<AssetIndex> m_index;
};

struct BundleExtractStats {
    usize files = 0;
    usize bytes = 0;
    usize skipped = 0; // already present with the same size (incremental extraction)
};

// Copies every indexed file below `prefix` to `destination/<path relative to prefix>` (creating directories).
// Files that already exist with the same size are skipped unless `overwrite`. Used where code needs native paths
// (shader #includes, paks opened by path, UI fonts). Fails on the first unreadable/unwritable file.
[[nodiscard]] Result<BundleExtractStats> extractBundle(const IAssetBundle& bundle, const AssetIndex& index,
                                                       std::string_view prefix, const std::filesystem::path& destination,
                                                       bool overwrite = false);

} // namespace ox
