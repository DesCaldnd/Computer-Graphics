#pragma once

// Editor-side asset database for one project:
//   <project>/Assets/**            sources + "<file>.meta" sidecars (UUID, importer, settings)
//   <project>/.oxcache/artifacts/  cooked artifacts "<uuid><ext>" (.oxmesh, .oxtex, .oxmat, ...)
//   <project>/.oxcache/imports/    "<uuid>.json" import records (hashes, artifacts, dependencies)
//
// scan() creates missing metas, maps UUID <-> path, detects moves (meta moved with the file, or an orphan meta
// whose recorded source hash matches a new meta-less file) and fixes duplicate UUIDs (copied metas).
// Importing is lazy (record()/readArtifact() import when stale) or explicit (importAll()). With watching
// enabled, poll() reimports changed sources/metas and emits onReimported for the asset and its dependents.
// Thread-safe (one recursive mutex; imports serialise).

#include <oxwald/assets/asset_source.hpp>
#include <oxwald/assets/importer.hpp>
#include <oxwald/core/events.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>

namespace ox {
class FileWatcher;
}

namespace ox::assets {

struct AssetInfo {
    Uuid uuid;
    AssetType type = AssetType::Unknown;
    std::string path;     // relative to Assets/, '/' separators; sub-assets: "<parent path>#<name>"
    std::string importer; // empty for sub-assets
    Uuid parent;          // main asset of a sub-asset
    std::string subName;
    std::vector<Uuid> subAssets;
    std::vector<Uuid> dependencies;
    std::filesystem::path artifact; // absolute, empty until imported
    bool imported = false;
    nlohmann::ordered_json info;
};

struct ScanResult {
    u32 found = 0;
    u32 metasCreated = 0;
    u32 moved = 0;
    u32 removed = 0;
    u32 duplicatesFixed = 0;
};

struct ImportStats {
    u32 imported = 0;
    u32 upToDate = 0;
    u32 failed = 0;
};

class AssetRegistry final : public IAssetSource {
public:
    struct Options {
        bool deleteOrphanMetas = true; // metas whose source vanished (and was not matched to a moved file)
        bool importOnDemand = true;    // record()/readArtifact() import stale assets
        std::chrono::milliseconds watchDebounce{100};
    };

    explicit AssetRegistry(std::filesystem::path projectDir);
    AssetRegistry(std::filesystem::path projectDir, Options options);
    ~AssetRegistry() override;
    AssetRegistry(const AssetRegistry&) = delete;
    AssetRegistry& operator=(const AssetRegistry&) = delete;

    [[nodiscard]] ImporterRegistry& importers() { return m_importers; }
    [[nodiscard]] const std::filesystem::path& projectDir() const { return m_projectDir; }
    [[nodiscard]] const std::filesystem::path& assetsDir() const { return m_assetsDir; }
    [[nodiscard]] const std::filesystem::path& cacheDir() const { return m_cacheDir; }

    ScanResult scan();
    ImportStats importAll(bool force = false);
    // Imports one main asset (sub-assets are imported with their parent).
    Status import(const Uuid& uuid, bool force = false);
    [[nodiscard]] bool needsImport(const Uuid& uuid);
    // Changes importer settings in the meta and reimports.
    Status setSettings(const Uuid& uuid, const nlohmann::ordered_json& settings);

    [[nodiscard]] std::optional<AssetInfo> info(const Uuid& uuid);
    [[nodiscard]] std::optional<AssetMeta> meta(const Uuid& uuid);
    [[nodiscard]] std::filesystem::path absolutePath(const Uuid& uuid);
    [[nodiscard]] std::vector<Uuid> dependencies(const Uuid& uuid);
    // Assets depending on uuid (directly).
    [[nodiscard]] std::vector<Uuid> dependents(const Uuid& uuid);
    // Transitive closure (sub-assets of models included), uuid first.
    [[nodiscard]] std::vector<Uuid> collectDependencies(std::span<const Uuid> roots);

    // IAssetSource
    [[nodiscard]] std::optional<AssetRecord> record(const Uuid& uuid) override;
    [[nodiscard]] std::optional<Uuid> uuidForPath(std::string_view path) override;
    [[nodiscard]] Result<std::vector<std::byte>> readArtifact(const Uuid& uuid) override;
    [[nodiscard]] Result<std::vector<std::byte>> readArtifactRange(const Uuid& uuid, u64 offset, u64 size) override;
    [[nodiscard]] std::vector<Uuid> allAssets() override;
    Connection subscribeChanges(std::function<void(const Uuid&)> fn) override;

    // Hot reload: watch Assets/ (polling FileWatcher); poll() handles changes on the calling thread.
    void startWatching();
    void stopWatching();
    // Returns the number of reimported assets.
    usize poll();

    Signal<const Uuid&> onReimported;                                       // main + sub-assets + dependents
    Signal<const Uuid&, const std::string&, const std::string&> onMoved;    // uuid, old path, new path
    Signal<const Uuid&> onRemoved;

private:
    struct Entry;
    struct ImportRecord;

    Entry* findEntry(const Uuid& uuid);
    Status importLocked(Entry& entry, bool force, std::vector<Uuid>* changed);
    bool isStaleLocked(const Entry& entry);
    std::optional<Uuid> resolveForImport(const std::filesystem::path& file, const nlohmann::ordered_json& hint);
    Entry* addSourceLocked(const std::filesystem::path& absPath, bool* createdMeta);
    void loadRecordLocked(Entry& entry);
    void rebuildDependentsLocked();
    std::string relativePath(const std::filesystem::path& abs) const;
    void handleChangeLocked(const std::filesystem::path& path, std::vector<Uuid>& changed);

    std::filesystem::path m_projectDir;
    std::filesystem::path m_assetsDir;
    std::filesystem::path m_cacheDir;
    Options m_options;
    ImporterRegistry m_importers;

    std::recursive_mutex m_mutex;
    std::unordered_map<Uuid, std::unique_ptr<Entry>> m_entries;
    std::unordered_map<std::string, Uuid> m_pathToUuid; // relative path -> main asset
    std::unordered_map<Uuid, std::set<Uuid>> m_dependents;
    std::unordered_map<std::string, std::set<Uuid>> m_sourceDepToAssets; // abs extra source -> importing assets

    std::unique_ptr<FileWatcher> m_watcher;
    std::mutex m_changeMutex;
    std::vector<std::filesystem::path> m_pendingChanges;
    Signal<const Uuid&> m_changed; // IAssetSource subscription
};

} // namespace ox::assets
