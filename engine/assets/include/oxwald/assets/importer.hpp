#pragma once

// Importer plug-in interface. Built-ins: texture, cubemap (.oxcube), model (glTF/FBX/OBJ/...), material
// (.oxmat), scene, prefab, script, audio, font, navmesh, heightmap. Other modules add their own:
//
//   registry.importers().add(std::make_unique<MyImporter>());

#include <oxwald/assets/asset_meta.hpp>
#include <oxwald/assets/asset_types.hpp>
#include <oxwald/core/result.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ox::assets {

struct ImportedArtifact {
    Uuid uuid;
    AssetType type = AssetType::Unknown;
    std::string name; // "" for the main asset, otherwise the sub-asset name (stable across reimports)
    std::vector<std::byte> data;
    std::vector<Uuid> dependencies;
    nlohmann::ordered_json info = nlohmann::ordered_json::object(); // stats shown by tools/editor
};

class ImportContext {
public:
    struct Callbacks {
        // Resolves a source file (absolute path) to its asset UUID, creating its meta when missing. The settings
        // hint is merged into the default settings of a newly created meta only.
        std::function<std::optional<Uuid>(const std::filesystem::path&, const nlohmann::ordered_json&)> resolve;
    };

    ImportContext(std::filesystem::path source, std::string assetPath, AssetMeta meta, Callbacks callbacks = {});

    [[nodiscard]] const std::filesystem::path& sourcePath() const { return m_source; }
    [[nodiscard]] const std::string& assetPath() const { return m_assetPath; }
    [[nodiscard]] const AssetMeta& meta() const { return m_meta; }
    [[nodiscard]] const Uuid& uuid() const { return m_meta.uuid; }
    [[nodiscard]] Result<std::vector<std::byte>> readSource() const;

    template <class T>
    [[nodiscard]] T settings() const {
        T s{};
        fromSettingsJson(m_meta.settings, s);
        return s;
    }

    ImportedArtifact& setMain(AssetType type, std::vector<std::byte> data);
    // Deterministic UUID: Uuid::fromName("<main uuid>/<name>").
    ImportedArtifact& addSubAsset(std::string name, AssetType type, std::vector<std::byte> data);
    [[nodiscard]] static Uuid subAssetUuid(const Uuid& main, std::string_view name);
    // Extra source files (.bin, .mtl, textures) whose change triggers a reimport.
    void addSourceDependency(const std::filesystem::path& file);
    [[nodiscard]] std::optional<Uuid> resolveAsset(const std::filesystem::path& file,
                                                   const nlohmann::ordered_json& settingsHint = {});
    void warn(std::string message);

    [[nodiscard]] std::vector<ImportedArtifact>& artifacts() { return m_artifacts; }
    [[nodiscard]] const std::vector<ImportedArtifact>& artifacts() const { return m_artifacts; }
    [[nodiscard]] const std::vector<std::filesystem::path>& sourceDependencies() const { return m_sourceDeps; }
    [[nodiscard]] const std::vector<std::string>& warnings() const { return m_warnings; }

private:
    std::filesystem::path m_source;
    std::string m_assetPath;
    AssetMeta m_meta;
    Callbacks m_callbacks;
    std::vector<ImportedArtifact> m_artifacts;
    std::vector<std::filesystem::path> m_sourceDeps;
    std::vector<std::string> m_warnings;
};

class IAssetImporter {
public:
    virtual ~IAssetImporter() = default;
    [[nodiscard]] virtual std::string_view name() const = 0;
    // Bump to force reimport of every asset of this importer.
    [[nodiscard]] virtual u32 version() const = 0;
    // Lower-case, with dot; multi-part allowed (".oxscene.json").
    [[nodiscard]] virtual std::vector<std::string> extensions() const = 0;
    [[nodiscard]] virtual AssetType mainType() const = 0;
    [[nodiscard]] virtual nlohmann::ordered_json defaultSettings() const { return nlohmann::ordered_json::object(); }
    // Settings for a new meta: may specialise by file name (e.g. "*_normal.png" -> Normal texture).
    [[nodiscard]] virtual nlohmann::ordered_json defaultSettingsFor(const std::filesystem::path& source) const {
        return defaultSettings();
    }
    virtual Status import(ImportContext& ctx) = 0;
};

class ImporterRegistry {
public:
    ImporterRegistry() = default;
    ImporterRegistry(const ImporterRegistry&) = delete;
    ImporterRegistry& operator=(const ImporterRegistry&) = delete;

    // Later registrations win for the same extension.
    void add(std::unique_ptr<IAssetImporter> importer);
    [[nodiscard]] IAssetImporter* find(std::string_view name) const;
    // Longest matching extension, case-insensitive.
    [[nodiscard]] IAssetImporter* findForPath(const std::filesystem::path& path) const;
    [[nodiscard]] const std::vector<std::unique_ptr<IAssetImporter>>& all() const { return m_importers; }

    void addBuiltins();

private:
    std::vector<std::unique_ptr<IAssetImporter>> m_importers;
};

// Runs an importer outside a project (tools/oximport, tests): resolve callbacks create no metas.
[[nodiscard]] Result<std::vector<ImportedArtifact>> importStandalone(const ImporterRegistry& importers,
                                                                     const std::filesystem::path& source,
                                                                     const nlohmann::ordered_json& settings = {});

} // namespace ox::assets
