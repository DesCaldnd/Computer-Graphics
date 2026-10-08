#pragma once

#include <oxwald/core/uuid.hpp>

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace ox::editor {

struct AssetInfo {
    QString path;         // absolute
    QString relativePath; // relative to the content root, '/' separators
    QString name;         // file name without extension(s)
    QString type;         // "Folder", "Scene", "Prefab", "Mesh", "Material", "Texture", "Audio", "Script", ...
    Uuid uuid;
    bool isFolder = false;
    qint64 size = 0;
    QDateTime modified;
};

// The content browser talks to assets through this interface. FileSystemAssetBackend works on plain files (UUID
// from a sidecar .meta when present, else derived from the relative path); the assets module provides an
// AssetRegistry-backed implementation (import, .meta, dependency tracking) registered in EditorServices.
class IAssetBackend {
public:
    virtual ~IAssetBackend() = default;
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
};

class FileSystemAssetBackend final : public IAssetBackend {
public:
    explicit FileSystemAssetBackend(QString root = {});
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

private:
    [[nodiscard]] QString uniquePath(const QString& folder, const QString& baseName, const QString& suffix) const;
    QString m_root;
};

} // namespace ox::editor
