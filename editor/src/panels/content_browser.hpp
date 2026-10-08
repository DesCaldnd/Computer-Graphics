#pragma once

#include "content/asset_backend.hpp"

#include <QAbstractListModel>
#include <QFileSystemWatcher>
#include <QHash>
#include <QPixmap>
#include <QTimer>
#include <QWidget>

class QFileSystemModel;
class QHBoxLayout;
class QListView;
class QSortFilterProxyModel;
class QToolButton;
class QTreeView;
class QComboBox;

namespace ox::editor {

class EditorContext;
class SearchField;

class AssetListModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role { InfoRole = Qt::UserRole + 1, TypeRole, PathRole, ThumbRole };
    AssetListModel(EditorContext* ctx, QObject* parent = nullptr);

    void setFolder(const QString& folder);
    [[nodiscard]] QString folder() const { return m_folder; }
    void setFilter(const QString& text, const QString& type);
    void refresh();
    [[nodiscard]] const AssetInfo* at(const QModelIndex& i) const;
    [[nodiscard]] QModelIndex indexOfPath(const QString& path) const;

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;
    Qt::DropActions supportedDropActions() const override { return Qt::MoveAction; }
    bool canDropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column, const QModelIndex& parent) const override;
    bool dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column, const QModelIndex& parent) override;

    static QColor typeColor(const QString& type);
    // Copies + imports external files into `folder` (Finder drops, Import…). Returns the new paths.
    QStringList importFiles(const QStringList& files, const QString& folder);

private:
    QPixmap thumbnail(const AssetInfo& a) const;
    EditorContext* m_ctx;
    QString m_folder;
    QString m_text;
    QString m_type;
    QList<AssetInfo> m_all;
    QList<AssetInfo> m_items;
};

class ContentBrowserPanel : public QWidget {
    Q_OBJECT
public:
    explicit ContentBrowserPanel(EditorContext* ctx, QWidget* parent = nullptr);

    void navigate(const QString& folder);
    void setRoot(const QString& root);
    [[nodiscard]] AssetListModel* model() const { return m_model; }
    [[nodiscard]] QListView* view() const { return m_list; }
    void setTileMode(bool tiles);
    void deleteSelected();
    // Tests: delete without the confirmation dialog.
    void setConfirmDelete(bool confirm) { m_confirmDelete = confirm; }

Q_SIGNALS:
    void openSceneRequested(const QString& path);

private:
    void rebuildBreadcrumb();
    void activate(const QModelIndex& index);
    void showContextMenu(const QPoint& pos);
    void createAsset(const QString& type);
    void importFiles();

    EditorContext* m_ctx;
    QFileSystemModel* m_dirs;
    QTreeView* m_tree;
    QListView* m_list;
    AssetListModel* m_model;
    SearchField* m_search;
    QComboBox* m_typeFilter;
    QWidget* m_breadcrumb;
    QHBoxLayout* m_crumbLayout;
    QToolButton* m_gridButton;
    QToolButton* m_listButton;
    QFileSystemWatcher m_watcher;
    QTimer m_rescanTimer;
    QString m_root;
    bool m_confirmDelete = true;
};

} // namespace ox::editor
