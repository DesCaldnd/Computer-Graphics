#pragma once

#include <QTimer>
#include <QWidget>

class QLabel;
class QTreeWidget;

namespace ox::editor {

class EditorContext;

// Behaviour tree debugger for the selected entity: while playing, the running tree's nodes with their live status
// (BehaviorTree::trace(): Running / Success / Failure / Idle, ticked this frame in bold) and the blackboard; in
// edit mode, the static structure of the tree definition (asset or inline JSON).
class BehaviorTreePanel : public QWidget {
    Q_OBJECT
public:
    explicit BehaviorTreePanel(EditorContext* ctx, QWidget* parent = nullptr);
    void refresh();
    [[nodiscard]] QTreeWidget* nodes() const { return m_nodes; }
    [[nodiscard]] QTreeWidget* blackboard() const { return m_blackboard; }

protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;

private:
    EditorContext* m_ctx;
    QLabel* m_title;
    QLabel* m_status;
    QTreeWidget* m_nodes;
    QTreeWidget* m_blackboard;
    QTimer m_timer;
    QString m_signature;
};

} // namespace ox::editor
