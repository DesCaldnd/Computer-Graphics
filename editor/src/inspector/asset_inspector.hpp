#pragma once

#include "content/asset_backend.hpp"

#include <QWidget>

#include <optional>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QVBoxLayout;

namespace ox::editor {

class EditorContext;
class ReflectedObjectEditor;

// Inspector page for an asset selected in the Content Browser: header + thumbnail, import settings edited through
// reflection (TextureImportSettings, ModelImportSettings, ...) with Apply & Reimport / Reimport, material values
// for .oxmat files (MaterialAsset) with Save, importer stats, sub-assets, dependencies and dependents.
class AssetInspector : public QWidget {
    Q_OBJECT
public:
    explicit AssetInspector(EditorContext* ctx, QWidget* parent = nullptr);

    void setAsset(const QString& path);
    [[nodiscard]] const std::optional<AssetInfo>& asset() const { return m_asset; }
    [[nodiscard]] ReflectedObjectEditor* settingsEditor() const { return m_settings; }
    [[nodiscard]] ReflectedObjectEditor* materialEditor() const { return m_material; }
    // Writes the edited import settings into the .meta and reimports. False + status on failure.
    bool applyImportSettings();
    bool reimport();
    bool saveMaterial();

private:
    void rebuild();
    QWidget* assetList(const QList<AssetInfo>& assets, const QString& empty);

    EditorContext* m_ctx;
    QString m_path;
    std::optional<AssetInfo> m_asset;
    QVBoxLayout* m_layout = nullptr;
    QWidget* m_body = nullptr;
    ReflectedObjectEditor* m_settings = nullptr;
    QPlainTextEdit* m_rawSettings = nullptr;
    ReflectedObjectEditor* m_material = nullptr;
    QPushButton* m_applyButton = nullptr;
};

} // namespace ox::editor
