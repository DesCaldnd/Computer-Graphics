#pragma once

#include "core/common.hpp"

#include <QObject>

namespace ox::editor {

// Ordered set of selected entity UUIDs (UUIDs survive undo/redo re-creation and play-world cloning; entt
// handles do not). The last element is the primary selection (gizmo pivot, inspector header).
class Selection : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

    [[nodiscard]] const UuidList& ids() const { return m_ids; }
    [[nodiscard]] bool empty() const { return m_ids.empty(); }
    [[nodiscard]] usize size() const { return m_ids.size(); }
    [[nodiscard]] Uuid primary() const { return m_ids.empty() ? Uuid{} : m_ids.back(); }
    [[nodiscard]] bool contains(const Uuid& id) const;

    void set(const UuidList& ids);
    void select(const Uuid& id) { set(id.isNil() ? UuidList{} : UuidList{id}); }
    void add(const Uuid& id);
    void remove(const Uuid& id);
    void toggle(const Uuid& id);
    void clear() { set({}); }
    // Drops ids that no longer exist (predicate returns true for live ids).
    template <class Pred>
    void prune(Pred&& alive) {
        UuidList kept;
        for (const auto& id : m_ids) {
            if (alive(id)) kept.push_back(id);
        }
        if (kept.size() != m_ids.size()) set(kept);
    }

Q_SIGNALS:
    void changed();

private:
    UuidList m_ids;
};

} // namespace ox::editor
