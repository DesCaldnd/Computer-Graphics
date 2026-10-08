#pragma once

#include <oxwald/core/cvar.hpp>

#include <optional>
#include <string_view>
#include <vector>

// Scalability (quality presets, UE style). Each group has a level stored in the persisted int cvar
// "sg.<Group>" (e.g. "sg.Shadows"); setting it applies the per-level values of every cvar bound to that
// group. A group reports QualityLevel::Custom when any of its cvars was overridden away from the level value.
namespace ox::scalability {

using Level = QualityLevel;
using Group = Scalability;

[[nodiscard]] std::string_view groupName(Group group);
[[nodiscard]] std::optional<Group> groupFromName(std::string_view name);
[[nodiscard]] std::string_view levelName(Level level);
[[nodiscard]] std::optional<Level> levelFromName(std::string_view name);

void setOverall(Level level);
void setGroup(Group group, Level level);
[[nodiscard]] Level currentLevel(Group group);
// The common level of all groups, or Custom when they differ.
[[nodiscard]] Level overallLevel();
[[nodiscard]] std::vector<ICVar*> cvars(Group group);

// {"groups": {"Shadows": "High", ...}, "overrides": {"r.Shadows.Resolution": 1024}} — overrides hold cvars
// of Custom groups that differ from their group level.
[[nodiscard]] nlohmann::json savePreset();
bool loadPreset(const nlohmann::json& preset);

namespace detail {
void onCVarRegistered(ICVar& cvar);
} // namespace detail

} // namespace ox::scalability
