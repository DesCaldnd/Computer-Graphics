#include "core/selection.hpp"

#include <algorithm>

namespace ox::editor {

bool Selection::contains(const Uuid& id) const { return std::find(m_ids.begin(), m_ids.end(), id) != m_ids.end(); }

void Selection::set(const UuidList& ids) {
    UuidList unique;
    unique.reserve(ids.size());
    for (const auto& id : ids) {
        if (!id.isNil() && std::find(unique.begin(), unique.end(), id) == unique.end()) unique.push_back(id);
    }
    if (unique == m_ids) return;
    m_ids = std::move(unique);
    Q_EMIT changed();
}

void Selection::add(const Uuid& id) {
    if (id.isNil()) return;
    UuidList ids = m_ids;
    std::erase(ids, id);
    ids.push_back(id);
    set(ids);
}

void Selection::remove(const Uuid& id) {
    UuidList ids = m_ids;
    std::erase(ids, id);
    set(ids);
}

void Selection::toggle(const Uuid& id) {
    if (contains(id)) remove(id);
    else add(id);
}

} // namespace ox::editor
