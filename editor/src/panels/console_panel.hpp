#pragma once

#include <QAbstractListModel>
#include <QSortFilterProxyModel>
#include <QStringList>
#include <QWidget>

class QComboBox;
class QLineEdit;
class QListView;
class QToolButton;
class QCompleter;
class QStringListModel;

namespace ox::editor {

class EditorContext;
class SearchField;

class LogListModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role { LevelRole = Qt::UserRole + 1, CategoryRole, TimeRole };
    explicit LogListModel(QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
};

class LogFilterModel : public QSortFilterProxyModel {
    Q_OBJECT
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
    void setLevelEnabled(int level, bool on);
    [[nodiscard]] bool levelEnabled(int level) const { return (m_mask >> level) & 1u; }
    void setCategory(const QString& c);
    void setText(const QString& t);

protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override;

private:
    template <class Fn>
    void changeRowFilter(Fn&& change);
    unsigned m_mask = 0b111100; // Info, Warn, Error, Fatal
    QString m_category;
    QString m_text;
};

// Log viewer + console input (cvars and console commands with autocomplete and history).
class ConsolePanel : public QWidget {
    Q_OBJECT
public:
    explicit ConsolePanel(EditorContext* ctx, QWidget* parent = nullptr);
    // Executes a console line (also used by tests); returns the printed result.
    QString execute(const QString& line);
    [[nodiscard]] QLineEdit* input() const { return m_input; }
    [[nodiscard]] QListView* view() const { return m_view; }
    [[nodiscard]] const QStringList& history() const { return m_history; }
    // Absolute path of a "file.lua" mentioned in a log line (project root, Assets, scripts/), empty if unknown.
    [[nodiscard]] QString resolveSource(const QString& file) const;
    // Opens the source location under a viewport position of the log view (file:line links).
    bool openLinkAt(QPoint pos);

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void updateCounts();
    void refreshCategories();
    void refreshCompletions();

    EditorContext* m_ctx;
    LogListModel* m_model;
    LogFilterModel* m_filter;
    QListView* m_view;
    QLineEdit* m_input;
    SearchField* m_search;
    QComboBox* m_category;
    QToolButton* m_levelButtons[4] = {};
    QToolButton* m_autoScroll;
    QCompleter* m_completer;
    QStringListModel* m_completions;
    QStringList m_history;
    int m_historyPos = -1;
};

} // namespace ox::editor
