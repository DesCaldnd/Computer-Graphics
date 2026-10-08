#pragma once

#include <oxwald/core/serial/value.hpp>
#include <oxwald/core/uuid.hpp>

#include <QDateTime>
#include <QImage>
#include <QList>
#include <QString>
#include <QStringList>

#include <nlohmann/json.hpp>

#include <optional>

namespace ox::editor {

struct AssetInfo {
    QString path;         // absolute (sub-assets: the source file's path)
    QString relativePath; // relative to the content root, '/' separators (sub-assets: "<source>#<name>")
    QString name;         // file name without extension(s) (sub-assets: "<source name> · <sub name>")
    QString type;         // "Folder", "Scene", "Prefab", "Model", "Mesh", "Material", "Texture", "Audio", "Script",
                          // "BehaviorTree", "AnimatorController", "Animation", "Skeleton", "Shader", "Data", ...
    Uuid uuid;
    bool isFolder = false;
    bool isSubAsset = false;
    QString subName;      // sub-asset name ("Mesh/0")
    QString importer;     // importer name (asset database), empty for plain files
    bool imported = false;
    qint64 size = 0;
    QDateTime modified;
};

// Import settings of one asset: the importer name, the reflected settings type (e.g. "TextureImportSettings",
// empty when the importer has no reflected settings) and the settings as plain JSON (the .meta "settings").
struct ImportSettings {
    QString importer;
    QString typeName;
    nlohmann::json settings = nlohmann::json::object();
    nlohmann::json info = nlohmann::json::object(); // importer stats of the last import (vertex counts, sizes, ...)
};

// The content browser talks to assets through this interface. FileSystemAssetBackend works on plain files (UUID
// from a sidecar .meta when present, else derived from the relative path); RegistryAssetBackend (assets module)
// adapts the engine's AssetRegistry (import, .meta, sub-assets, dependency tracking, import settings).
class IAssetBackend {
public:
    virtual ~IAssetBackend() = default;
    [[nodiscard]] virtual QString name() const = 0;
    [[nodiscard]] virtual QString rootPath() const = 0;
    virtual void setRootPath(const QString& root) = 0;
    [[nodiscard]] virtual QList<AssetInfo> list(const QString& folder) const = 0; // folders first
    [[nodiscard]] virtual std::optional<AssetInfo> info(const QString& path) const = 0;
    [[nodiscard]] virtual std::optional<AssetInfo> find(const Uuid& id) const = 0;
    [[nodiscard]] virtual QStringList creatableTypes() const = 0;
    // Returns the path of the created item or an empty string (error set).
    virtual QString createFolder(const QString& parent, const QString& name, QString* error) = 0;
    virtual QString createAsset(const QString& folder, const QString& type, const QString& name, QString* error) = 0;
    virtual QString rename(const QString& path, const QString& newName, QString* error) = 0;
    virtual bool remove(const QString& path, QString* error) = 0;
    virtual QString move(const QString& path, const QString& destinationFolder, QString* error) = 0;
    [[nodiscard]] virtual QString typeForPath(const QString& path) const = 0;

    // ---- asset database features (defaults: plain files) ----
    [[nodiscard]] virtual bool isDatabase() const { return false; }
    // Copies external files (Finder drops, Import…) into `folder` and imports them. Returns the new paths.
    virtual QStringList importFiles(const QStringList& sources, const QString& folder, QString* error);
    virtual bool reimport(const QString& path, QString* error) {
        (void)path;
        (void)error;
        return true;
    }
    // Picks up external file changes (new files, moves) — the asset registry's scan().
    virtual void rescan() {}
    [[nodiscard]] virtual QList<AssetInfo> dependents(const QString& path) const {
        (void)path;
        return {};
    }
    [[nodiscard]] virtual QList<AssetInfo> dependencies(const QString& path) const {
        (void)path;
        return {};
    }
    [[nodiscard]] virtual QList<AssetInfo> subAssets(const QString& path) const {
        (void)path;
        return {};
    }
    // Every asset of `type` (asset reference pickers); includes sub-assets for databases.
    [[nodiscard]] virtual QList<AssetInfo> allOfType(const QString& type) const;
    [[nodiscard]] virtual std::optional<ImportSettings> importSettings(const QString& path) const {
        (void)path;
        return std::nullopt;
    }
    // Rewrites the .meta settings and reimports.
    virtual bool setImportSettings(const QString& path, const nlohmann::json& settings, QString* error) {
        (void)path;
        (void)settings;
        if (error) *error = QStringLiteral("not an asset database");
        return false;
    }
    // Prefab document of a .oxprefab file or of an imported model (its prefab artifact).
    [[nodiscard]] virtual std::optional<serial::Document> loadPrefabDocument(const QString& path, QString* error) const;
    // Preview decoded from the imported artifact (textures Qt cannot read: KTX2, EXR, HDR, BC data). Null = none.
    [[nodiscard]] virtual QImage decodedPreview(const AssetInfo& asset, int sizePx) const {
        (void)asset;
        (void)sizePx;
        return {};
    }
    // Directory for cached thumbnail PNGs ("<uuid>.png"); empty = memory cache only.
    [[nodiscard]] virtual QString thumbnailCacheDir() const { return {}; }
    // An asset usable where `wantedType` is expected (a Model dropped on a Mesh field -> its first Mesh sub-asset).
    [[nodiscard]] virtual Uuid resolveReference(const Uuid& id, const QString& assetType, const QString& wantedType) const;
};

// Type compatibility of asset references (reflection AssetRef names vs browser types: "AudioClip" ~ "Audio",
// "AnimationClip" ~ "Animation", "Prefab" accepts "Model").
[[nodiscard]] bool assetTypeMatches(const QString& assetType, const QString& wantedType);

class FileSystemAssetBackend final : public IAssetBackend {
public:
    explicit FileSystemAssetBackend(QString root = {});
    [[nodiscard]] QString name() const override { return QStringLiteral("Files"); }
    [[nodiscard]] QString rootPath() const override { return m_root; }
    void setRootPath(const QString& root) override { m_root = root; }
    [[nodiscard]] QList<AssetInfo> list(const QString& folder) const override;
    [[nodiscard]] std::optional<AssetInfo> info(const QString& path) const override;
    [[nodiscard]] std::optional<AssetInfo> find(const Uuid& id) const override;
    [[nodiscard]] QStringList creatableTypes() const override;
    QString createFolder(const QString& parent, const QString& name, QString* error) override;
    QString createAsset(const QString& folder, const QString& type, const QString& name, QString* error) override;
    QString rename(const QString& path, const QString& newName, QString* error) override;
    bool remove(const QString& path, QString* error) override;
    QString move(const QString& path, const QString& destinationFolder, QString* error) override;
    [[nodiscard]] QString typeForPath(const QString& path) const override;

    // Extension(s) -> type table used by typeForPath ("oxscene" -> Scene, "png" -> Texture, ...).
    [[nodiscard]] static QString typeForSuffix(const QString& fileName);
    [[nodiscard]] static QString uniquePath(const QString& folder, const QString& baseName, const QString& suffix);
    // Writes the file of a new asset of `type` (Scene, Prefab, Material, Script, BehaviorTree). Returns the path.
    static QString writeNewAsset(const QString& folder, const QString& type, const QString& name, QString* error);

private:
    QString m_root;
};

} // namespace ox::editor
