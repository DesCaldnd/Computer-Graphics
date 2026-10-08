#pragma once

#include <QTimer>
#include <QWidget>

class QLabel;
class QToolButton;
class QTreeWidget;

namespace ox::editor {

class EditorContext;
class SearchField;

// Live view of the engine's CoroutineScheduler (async module): name, owner entity, state, what it waits on, age
// (game seconds / frames); child coroutines nest under their parent. Cancel stops the selected coroutines
// (CoroutineHandle::cancel through handlesForOwner), double-click selects the owner entity.
class CoroutinesPanel : public QWidget {
    Q_OBJECT
public:
    explicit CoroutinesPanel(EditorContext* ctx, QWidget* parent = nullptr);

    void refresh();
    [[nodiscard]] QTreeWidget* tree() const { return m_tree; }
    // Number of coroutines listed by the last refresh.
    [[nodiscard]] int count() const { return m_count; }
    // Cancels the coroutine with this id (must have an owner). Returns true if found.
    bool cancel(unsigned long long id, unsigned long long owner);

protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;

private:
    EditorContext* m_ctx;
    QTreeWidget* m_tree;
    SearchField* m_search;
    QLabel* m_summary;
    QToolButton* m_cancel;
    QToolButton* m_pause;
    QTimer m_timer;
    int m_count = 0;
};

} // namespace ox::editor
