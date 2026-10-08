#pragma once

#include <QAction>
#include <QKeySequence>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QString>

namespace ox::editor {

// Named editor actions with default shortcuts; the keyboard shortcut editor rebinds them (overrides are stored
// in the user preferences).
class ActionRegistry : public QObject {
    Q_OBJECT
public:
    struct Entry {
        QString id;       // "file.save"
        QString category; // "File"
        QPointer<QAction> action;
        QKeySequence defaultShortcut;
    };

    using QObject::QObject;

    QAction* add(const QString& id, const QString& category, QAction* action, const QKeySequence& defaultShortcut = {});
    [[nodiscard]] QAction* action(const QString& id) const;
    [[nodiscard]] const QList<Entry>& entries() const { return m_entries; }
    [[nodiscard]] QKeySequence defaultShortcut(const QString& id) const;
    // Applies overrides (id -> sequence); ids missing from the map get their default.
    void applyOverrides(const QMap<QString, QKeySequence>& overrides);
    // Other action ids that use the same sequence.
    [[nodiscard]] QStringList conflicts(const QString& id, const QKeySequence& seq) const;
    void clear() { m_entries.clear(); }

private:
    QList<Entry> m_entries;
};

} // namespace ox::editor
