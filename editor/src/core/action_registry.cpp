#include "core/action_registry.hpp"

namespace ox::editor {

QAction* ActionRegistry::add(const QString& id, const QString& category, QAction* action, const QKeySequence& defaultShortcut) {
    for (auto& e : m_entries) {
        if (e.id == id) {
            e.action = action;
            e.defaultShortcut = defaultShortcut;
            action->setShortcut(defaultShortcut);
            return action;
        }
    }
    action->setShortcut(defaultShortcut);
    action->setObjectName(id);
    m_entries.push_back({id, category, action, defaultShortcut});
    return action;
}

QAction* ActionRegistry::action(const QString& id) const {
    for (const auto& e : m_entries) {
        if (e.id == id) return e.action;
    }
    return nullptr;
}

QKeySequence ActionRegistry::defaultShortcut(const QString& id) const {
    for (const auto& e : m_entries) {
        if (e.id == id) return e.defaultShortcut;
    }
    return {};
}

void ActionRegistry::applyOverrides(const QMap<QString, QKeySequence>& overrides) {
    for (auto& e : m_entries) {
        if (!e.action) continue;
        e.action->setShortcut(overrides.contains(e.id) ? overrides.value(e.id) : e.defaultShortcut);
    }
}

QStringList ActionRegistry::conflicts(const QString& id, const QKeySequence& seq) const {
    QStringList out;
    if (seq.isEmpty()) return out;
    for (const auto& e : m_entries) {
        if (e.id != id && e.action && e.action->shortcut() == seq) out << e.id;
    }
    return out;
}

} // namespace ox::editor
