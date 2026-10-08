#pragma once

#include "core/common.hpp"

#include <QAbstractItemModel>
#include <QSortFilterProxyModel>
#include <QTimer>
#include <QTreeView>
#include <QWidget>

#include <memory>
#include <unordered_map>
#include <vector>

class QLabel;
class QMenu;

namespace ox::editor {

class EditorContext;
class SearchField;

// Hierarchy of the shown World (edit or play) keyed by UUID. Column 0 name, 1 visibility, 2 lock.
class OutlinerModel : public QAbstractItemModel {
    Q_OBJECT
public:
    enum Column { NameColumn = 0, VisibleColumn, LockColumn, ColumnCount };
    enum Role { UuidRole = Qt::UserRole + 1, TypeRole };

    explicit OutlinerModel(EditorContext* ctx, QObject* parent = nullptr);

    void rebuild();
    [[nodiscard]] QModelIndex indexOf(const Uuid& id, int column = 0) const;
    [[nodiscard]] Uuid idOf(const QModelIndex& index) const;
    [[nodiscard]] int entityCount() const { return int(m_byId.size()); }

    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override { (void)parent; return ColumnCount; }
    QVariant data(const QModelIndex& index, int role) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    Qt::DropActions supportedDropActions() const override { return Qt::MoveAction | Qt::CopyAction; }
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;
    bool canDropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column, const QModelIndex& parent) const override;
    bool dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column, const QModelIndex& parent) override;

private:
    struct Node {
        Uuid id;
        Node* parent = nullptr;
        std::vector<std::unique_ptr<Node>> children;
        int row = 0;
        QString name;
        QString icon;
        QString type;
        bool active = true;
        bool prefab = false;
    };
    Node* nodeOf(const QModelIndex& index) const;

    EditorContext* m_ctx;
    Node m_root;
    std::unordered_map<Uuid, Node*> m_byId;
};

class OutlinerPanel : public QWidget {
    Q_OBJECT
public:
    explicit OutlinerPanel(EditorContext* ctx, QWidget* parent = nullptr);

    [[nodiscard]] QTreeView* view() const { return m_view; }
    [[nodiscard]] OutlinerModel* model() const { return m_model; }
    void rebuildNow();
    void setFilterText(const QString& text);
    // Builds the "Create" menu (shared with the main window's Entity menu).
    static void populateCreateMenu(QMenu* menu, EditorContext* ctx, const Uuid& parent);
    void renameSelected();

protected:
    void changeEvent(QEvent* e) override;

private:
    void syncSelectionToView();
    void syncSelectionFromView();
    void showContextMenu(const QPoint& pos);
    void updateFooter();

    EditorContext* m_ctx;
    OutlinerModel* m_model;
    QSortFilterProxyModel* m_proxy;
    QTreeView* m_view;
    SearchField* m_search;
    QLabel* m_footer;
    QTimer m_rebuildTimer;
    bool m_syncing = false;
};

} // namespace ox::editor
