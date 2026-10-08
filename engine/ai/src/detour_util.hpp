#pragma once

#include <oxwald/ai/nav_types.hpp>

#include <DetourNavMeshQuery.h>

namespace ox::ai::detail {

inline void toDetour(const NavQueryFilter& f, dtQueryFilter& out) {
    for (int i = 0; i < DT_MAX_AREAS && i < NavArea::Count; ++i) {
        out.setAreaCost(i, f.areaCost[static_cast<usize>(i)]);
    }
    out.setIncludeFlags(f.includeFlags);
    out.setExcludeFlags(f.excludeFlags);
}

inline const float* ptr(const glm::vec3& v) { return &v.x; }
inline glm::vec3 vec(const float* p) { return {p[0], p[1], p[2]}; }

} // namespace ox::ai::detail
