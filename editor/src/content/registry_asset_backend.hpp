#pragma once

#include "content/asset_backend.hpp"

#if OX_EDITOR_HAS_ASSETS

namespace ox::assets {
class AssetRegistry;
class AssetManager;
} // namespace ox::assets

namespace ox::editor {

// Content browser backend on the engine's AssetRegistry (<project>/Assets + .meta + .oxcache): real asset types
// and UUIDs from the database, sub-assets of models, import on drop, reimport, import settings (.meta), renames and
// moves that keep the .meta next to the file (the registry keeps the UUID), dependency queries, previews decoded
// from imported artifacts and a thumbnail disk cache in .oxcache/thumbnails.
class RegistryAssetBackend final : public IAssetBackend {
public:
    RegistryAssetBackend(assets::AssetRegistry& registry, assets::AssetManager* manager);

    [[nodiscard]] QString name() const override { return QStringLiteral("Asset database"); }
    [[nodiscard]] QString rootPath() const override { return m_root; }
    void setRootPath(const QString&) override {} // fixed: the registry's Assets/ directory
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

    [[nodiscard]] bool isDatabase() const override { return true; }
    QStringList importFiles(const QStringList& sources, const QString& folder, QString* error) override;
    bool reimport(const QString& path, QString* error) override;
    void rescan() override;
    [[nodiscard]] QList<AssetInfo> dependents(const QString& path) const override;
    [[nodiscard]] QList<AssetInfo> dependencies(const QString& path) const override;
    [[nodiscard]] QList<AssetInfo> subAssets(const QString& path) const override;
    [[nodiscard]] QList<AssetInfo> allOfType(const QString& type) const override;
    [[nodiscard]] std::optional<ImportSettings> importSettings(const QString& path) const override;
    bool setImportSettings(const QString& path, const nlohmann::json& settings, QString* error) override;
    [[nodiscard]] std::optional<serial::Document> loadPrefabDocument(const QString& path, QString* error) const override;
    [[nodiscard]] QImage decodedPreview(const AssetInfo& asset, int sizePx) const override;
    [[nodiscard]] QString thumbnailCacheDir() const override;
    [[nodiscard]] Uuid resolveReference(const Uuid& id, const QString& assetType, const QString& wantedType) const override;

    [[nodiscard]] assets::AssetRegistry& registry() const { return m_registry; }
    // Editor type name of an asset of the database ("Texture", "Model", "BehaviorTree", ...).
    [[nodiscard]] static QString editorTypeName(int assetType, const QString& importer, const QString& kind);

private:
    [[nodiscard]] std::optional<Uuid> uuidOfPath(const QString& absolutePath) const;
    [[nodiscard]] std::optional<AssetInfo> infoForUuid(const Uuid& id) const;
    [[nodiscard]] QString relative(const QString& absolutePath) const;

    assets::AssetRegistry& m_registry;
    assets::AssetManager* m_manager;
    QString m_root;
};

} // namespace ox::editor

#endif
