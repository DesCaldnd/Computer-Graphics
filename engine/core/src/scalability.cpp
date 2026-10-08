#include <oxwald/core/scalability.hpp>

#include <array>
#include <memory>

namespace ox::scalability {
namespace {

constexpr std::array<std::string_view, kScalabilityGroupCount> kGroupNames = {
    "ViewDistance", "AntiAliasing", "Shadows", "GlobalIllumination", "Reflections", "PostProcess",
    "Textures",     "Effects",      "Foliage", "Shading",            "Volumetrics", "RayTracing",
};
constexpr std::array<std::string_view, kQualityLevelCount> kLevelNames = {"Low", "Medium", "High", "Ultra"};

void applyGroup(Group group, Level level) {
    for (ICVar* c : CVarRegistry::instance().inGroup(group)) c->applyLevel(level);
}

// The "sg.<Group>" cvars, created on first use so registration order across translation units does not matter.
struct GroupCVars {
    std::array<std::unique_ptr<CVar<int>>, kScalabilityGroupCount> cvars;

    GroupCVars() {
        for (usize i = 0; i < kScalabilityGroupCount; ++i) {
            const auto group = static_cast<Group>(i);
            cvars[i] = std::make_unique<CVar<int>>(
                "sg." + std::string(kGroupNames[i]), int(Level::High),
                "Scalability level of the " + std::string(kGroupNames[i]) + " group (Low, Medium, High, Ultra)",
                CVarEnum{"Low", "Medium", "High", "Ultra"}, CVarFlags::Persist);
            cvars[i]->onChanged([group](int now, int) { applyGroup(group, static_cast<Level>(now)); });
        }
    }
};

GroupCVars& groupCVars() {
    // Leaked: cvars must outlive other static objects that may query levels during shutdown.
    static GroupCVars* g = new GroupCVars();
    return *g;
}

CVar<int>& groupCVar(Group g) { return *groupCVars().cvars[usize(g)]; }

bool explicitlySet(Group g) { return groupCVar(g).lastSource() != CVarSource::Default; }

} // namespace

std::string_view groupName(Group group) {
    const auto i = usize(group);
    return i < kGroupNames.size() ? kGroupNames[i] : std::string_view("None");
}

std::optional<Group> groupFromName(std::string_view name) {
    for (usize i = 0; i < kGroupNames.size(); ++i) {
        if (kGroupNames[i] == name) return static_cast<Group>(i);
    }
    return std::nullopt;
}

std::string_view levelName(Level level) {
    if (level == Level::Custom) return "Custom";
    return kLevelNames[usize(level)];
}

std::optional<Level> levelFromName(std::string_view name) {
    if (name == "Custom") return Level::Custom;
    for (usize i = 0; i < kLevelNames.size(); ++i) {
        if (kLevelNames[i] == name) return static_cast<Level>(i);
    }
    return std::nullopt;
}

void setGroup(Group group, Level level) {
    if (level == Level::Custom || group == Group::None || group == Group::Count) return;
    auto& cv = groupCVar(group);
    // Apply even when the level is unchanged: it discards per-cvar overrides (back from Custom).
    applyGroup(group, level);
    cv.set(int(level), CVarSource::Code);
}

void setOverall(Level level) {
    for (usize i = 0; i < kScalabilityGroupCount; ++i) setGroup(static_cast<Group>(i), level);
}

Level currentLevel(Group group) {
    const auto members = cvars(group);
    if (explicitlySet(group)) {
        const auto level = static_cast<Level>(groupCVar(group).get());
        for (auto* c : members) {
            if (!c->matchesLevel(level)) return Level::Custom;
        }
        return level;
    }
    if (members.empty()) return static_cast<Level>(groupCVar(group).get());
    // Never set: infer from the current values (highest level that all members match).
    for (int l = int(kQualityLevelCount) - 1; l >= 0; --l) {
        bool all = true;
        for (auto* c : members) all = all && c->matchesLevel(static_cast<Level>(l));
        if (all) return static_cast<Level>(l);
    }
    return Level::Custom;
}

Level overallLevel() {
    const Level first = currentLevel(static_cast<Group>(0));
    for (usize i = 1; i < kScalabilityGroupCount; ++i) {
        if (currentLevel(static_cast<Group>(i)) != first) return Level::Custom;
    }
    return first;
}

std::vector<ICVar*> cvars(Group group) {
    groupCVars();
    return CVarRegistry::instance().inGroup(group);
}

nlohmann::json savePreset() {
    nlohmann::json groups = nlohmann::json::object();
    nlohmann::json overrides = nlohmann::json::object();
    for (usize i = 0; i < kScalabilityGroupCount; ++i) {
        const auto g = static_cast<Group>(i);
        const Level cur = currentLevel(g);
        const auto base = static_cast<Level>(groupCVar(g).get());
        groups[std::string(kGroupNames[i])] = std::string(levelName(cur == Level::Custom ? base : cur));
        if (cur == Level::Custom) {
            for (auto* c : cvars(g)) {
                if (!c->matchesLevel(base)) overrides[c->name()] = c->toJson();
            }
        }
    }
    return nlohmann::json{{"groups", groups}, {"overrides", overrides}};
}

bool loadPreset(const nlohmann::json& preset) {
    if (!preset.is_object()) return false;
    bool ok = true;
    if (auto it = preset.find("groups"); it != preset.end() && it->is_object()) {
        for (const auto& [name, level] : it->items()) {
            auto g = groupFromName(name);
            auto l = level.is_string() ? levelFromName(level.get<std::string>()) : std::nullopt;
            if (!g || !l || *l == Level::Custom) {
                ok = false;
                continue;
            }
            setGroup(*g, *l);
        }
    }
    if (auto it = preset.find("overrides"); it != preset.end() && it->is_object()) {
        for (const auto& [name, value] : it->items()) {
            ICVar* c = CVarRegistry::instance().find(name);
            ok = (c && c->setFromJson(value, CVarSource::Config)) && ok;
        }
    }
    return ok;
}

namespace detail {
void onCVarRegistered(ICVar& cvar) {
    const Group g = cvar.group();
    if (g == Group::None || g == Group::Count || !cvar.hasLevelValues()) return;
    // Cvars registered after a level was chosen (e.g. a module loaded later) start at that level.
    if (explicitlySet(g)) cvar.applyLevel(static_cast<Level>(groupCVar(g).get()));
}
} // namespace detail

} // namespace ox::scalability
