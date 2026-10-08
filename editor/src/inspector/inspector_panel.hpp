#pragma once

#include "core/common.hpp"

#include <QFrame>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <vector>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QTreeWidget;
class QVBoxLayout;

namespace ox::editor {

class AssetInspector;
class ComponentCard;
class EditorContext;
class SearchField;
class ToggleSwitch;

// Searchable "Add Component" popup grouped by ComponentRegistry category.
class AddComponentPopup : public QFrame {
    Q_OBJECT
public:
    AddComponentPopup(EditorContext* ctx, const UuidList& entities, QWidget* parent = nullptr);
    void popup(const QPoint& globalPos, int width);
    [[nodiscard]] QTreeWidget* tree() const { return m_tree; }

private:
    void rebuild(const QString& filter);
    void activate();
    EditorContext* m_ctx;
    UuidList m_entities;
    SearchField* m_search;
    QTreeWidget* m_tree;
};

class InspectorPanel : public QWidget {
    Q_OBJECT
public:
    explicit InspectorPanel(EditorContext* ctx, QWidget* parent = nullptr);

    // Immediate rebuild/refresh (normally coalesced through a zero-timeout timer).
    void rebuild();
    void refresh();
    [[nodiscard]] const std::vector<ComponentCard*>& cards() const { return m_cards; }
    [[nodiscard]] ComponentCard* card(const QString& componentName) const;
    [[nodiscard]] QPushButton* addComponentButton() const { return m_addButton; }
    [[nodiscard]] AssetInspector* assetPage() const { return m_assetPage; }

protected:
    void changeEvent(QEvent* e) override;

private:
    void updateHeader();
    [[nodiscard]] QString componentSetSignature() const;

    EditorContext* m_ctx;
    QStackedWidget* m_stack;
    QWidget* m_empty;
    QWidget* m_content;
    AssetInspector* m_assetPage = nullptr;
    QLabel* m_entityIcon;
    QLineEdit* m_nameEdit;
    QCheckBox* m_activeBox;
    QLabel* m_idLabel;
    QFrame* m_prefabBanner;
    QLabel* m_prefabLabel;
    SearchField* m_filter;
    QPushButton* m_addButton;
    QScrollArea* m_scroll;
    QVBoxLayout* m_cardsLayout;
    std::vector<ComponentCard*> m_cards;
    UuidList m_shown;
    QString m_signature;
    QTimer m_refreshTimer;
    QTimer m_rebuildTimer;
};

} // namespace ox::editor
