#include <oxwald/core/log.hpp>
#include <oxwald/core/profile.hpp>
#include <oxwald/core/reflect.hpp>
#include <oxwald/core/scalability.hpp>
#include <oxwald/core/vfs.hpp>
#include <oxwald/runtime/json_io.hpp>
#include <oxwald/runtime/settings.hpp>

#include <algorithm>
#include <array>
#include <set>

namespace ox {

namespace cvars {
CVar<bool>& vsync() {
    static CVar<bool> cv("r.VSync", true, "Vertical sync", CVarFlags::Persist);
    return cv;
}
CVar<int>& windowMode() {
    static CVar<int> cv("r.WindowMode", 0, "Window mode", CVarEnum{"Windowed", "Borderless", "Fullscreen"},
                        CVarFlags::Persist);
    return cv;
}
CVar<int>& resolutionX() {
    static CVar<int> cv("r.ResolutionX", 0, "Window/fullscreen width (0 = default)", 0, 16384, CVarFlags::Persist);
    return cv;
}
CVar<int>& resolutionY() {
    static CVar<int> cv("r.ResolutionY", 0, "Window/fullscreen height (0 = default)", 0, 16384, CVarFlags::Persist);
    return cv;
}
CVar<int>& monitor() {
    static CVar<int> cv("r.Monitor", 0, "Monitor index for fullscreen", 0, 16, CVarFlags::Persist);
    return cv;
}
CVar<int>& maxFps() {
    static CVar<int> cv("t.MaxFPS", 0, "Frame rate limit (0 = unlimited)", 0, 1000, CVarFlags::Persist);
    return cv;
}
CVar<float>& fov() {
    static CVar<float> cv("g.FOV", 90.0f, "Gameplay field of view (degrees)", 30.0f, 150.0f, CVarFlags::Persist);
    return cv;
}
CVar<float>& masterVolume() {
    static CVar<float> cv("a.MasterVolume", 1.0f, "Master volume", 0.0f, 1.0f, CVarFlags::Persist);
    return cv;
}
} // namespace cvars

namespace {

// Registers the runtime cvars at static initialisation (whenever this object file is linked) so the console and
// config files see them even before a Settings instance exists.
[[maybe_unused]] const bool kCVarsRegistered = [] {
    cvars::vsync();
    cvars::windowMode();
    cvars::resolutionX();
    cvars::resolutionY();
    cvars::monitor();
    cvars::maxFps();
    cvars::fov();
    cvars::masterVolume();
    return true;
}();

// Cvars mirrored by explicit UserSettings fields (not duplicated into UserSettings::cvars).
constexpr std::array<std::string_view, 11> kManagedCVars = {
    "r.VSync",   "r.WindowMode", "r.ResolutionX", "r.ResolutionY",     "r.Monitor",   "t.MaxFPS",
    "g.FOV",     "a.MasterVolume", "r.RayTracing", "r.Upscaler", "r.Upscaler.Quality"};

bool isManaged(std::string_view name) {
    return name.starts_with("sg.") || std::find(kManagedCVars.begin(), kManagedCVars.end(), name) != kManagedCVars.end();
}

void setByName(std::string_view name, const nlohmann::json& value) {
    nlohmann::json obj = nlohmann::json::object();
    obj[std::string(name)] = value;
    // Pending until registered when the owning module (render) is not linked yet.
    CVarRegistry::instance().loadOverrides(obj);
}

void applyQuality(const std::string& overall, const std::map<std::string, std::string>& groups) {
    if (auto level = scalability::levelFromName(overall); level && *level != QualityLevel::Custom) {
        scalability::setOverall(*level);
    } else if (overall != "Custom" && !overall.empty()) {
        OX_LOG_WARN("settings", "unknown quality level '{}'", overall);
    }
    for (const auto& [groupName, levelName] : groups) {
        auto g = scalability::groupFromName(groupName);
        auto l = scalability::levelFromName(levelName);
        if (g && l && *l != QualityLevel::Custom) {
            scalability::setGroup(*g, *l);
        } else {
            OX_LOG_WARN("settings", "ignoring scalability {}={}", groupName, levelName);
        }
    }
}

} // namespace

void registerSettingsTypes() {
    registerProjectTypes();
    OX_REFLECT_ENUM(WindowMode, "WindowMode")
        .value("Windowed", WindowMode::Windowed)
        .value("Borderless", WindowMode::Borderless)
        .value("Fullscreen", WindowMode::Fullscreen);
    OX_REFLECT_TYPE(GraphicsSettings, "GraphicsSettings")
        .field("resolution", &GraphicsSettings::resolution)
        .field("windowMode", &GraphicsSettings::windowMode)
        .field("monitor", &GraphicsSettings::monitor)
        .field("vsync", &GraphicsSettings::vsync)
        .field("maxFps", &GraphicsSettings::maxFps)
        .field("quality", &GraphicsSettings::quality)
        .field("groups", &GraphicsSettings::groups)
        .field("rayTracing", &GraphicsSettings::rayTracing)
        .field("upscaler", &GraphicsSettings::upscaler)
        .field("upscalerQuality", &GraphicsSettings::upscalerQuality)
        .field("fov", &GraphicsSettings::fov);
    OX_REFLECT_TYPE(AudioSettings, "AudioSettings")
        .field("masterVolume", &AudioSettings::masterVolume, attr::Range{0.0, 1.0})
        .field("busVolumes", &AudioSettings::busVolumes);
    OX_REFLECT_TYPE(UserSettings, "UserSettings")
        .field("graphics", &UserSettings::graphics)
        .field("audio", &UserSettings::audio)
        .field("inputRebinds", &UserSettings::inputRebinds)
        .field("mouseSensitivity", &UserSettings::mouseSensitivity)
        .field("invertY", &UserSettings::invertY)
        .field("language", &UserSettings::language)
        .field("cvars", &UserSettings::cvars);
}

Settings::Settings(Vfs* vfs, std::string uri) : m_vfs(vfs), m_uri(std::move(uri)) {
    registerSettingsTypes();
    // Register the runtime cvars now so the console sees them.
    cvars::vsync();
    cvars::windowMode();
    cvars::resolutionX();
    cvars::resolutionY();
    cvars::monitor();
    cvars::maxFps();
    cvars::fov();
    cvars::masterVolume();
}

void Settings::applyProjectDefaults() {
    OX_PROFILE_ZONE();
    applyQuality(m_project.defaultQuality, m_project.scalability);
    for (const auto& [name, value] : m_project.rendering.cvars) {
        if (ICVar* c = CVarRegistry::instance().find(name)) {
            c->setFromString(value, CVarSource::Config);
        } else {
            setByName(name, value);
        }
    }
    if (m_project.rendering.upscaler != "Off") setByName("r.Upscaler", m_project.rendering.upscaler);
    // The renderer ignores r.RayTracing on GPUs without ray queries, which makes this "if supported".
    if (m_project.rendering.rayTracingIfSupported) setByName("r.RayTracing", true);
}

void Settings::seedUserFromProject() {
    captureFromCVars();
    GraphicsSettings& g = m_user.graphics;
    if (m_project.rendering.rayTracingIfSupported) g.rayTracing = true;
    if (m_project.rendering.upscaler != "Off") g.upscaler = m_project.rendering.upscaler;
}

nlohmann::ordered_json Settings::toJson() { return json::toPlain(m_user); }

bool Settings::fromJson(const nlohmann::ordered_json& j) {
    UserSettings fresh;
    if (!json::fromPlain(j, fresh)) return false;
    m_user = std::move(fresh);
    return true;
}

Status Settings::load() {
    if (!m_vfs) return makeError("settings: no VFS");
    if (!m_vfs->exists(m_uri)) return {};
    auto text = m_vfs->readText(m_uri);
    if (!text) return text.error();
    auto j = json::parse(*text);
    if (!j || !fromJson(*j)) return makeError("settings: '{}' is not valid", m_uri);
    m_hasUserFile = true;
    return {};
}

Status Settings::save() {
    if (!m_vfs) return makeError("settings: no VFS");
    if (!m_vfs->writeText(m_uri, toJson().dump(2) + "\n")) return makeError("settings: cannot write '{}'", m_uri);
    return {};
}

Status Settings::loadFrom(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return {};
    auto j = json::loadFile(path);
    if (!j) return j.error();
    if (!fromJson(*j)) return makeError("settings: '{}' is not valid", path.string());
    return {};
}

Status Settings::saveTo(const std::filesystem::path& path) { return json::saveFile(path, toJson()); }

void Settings::apply(SettingsCategory categories) {
    OX_PROFILE_ZONE();
    if (hasCategory(categories, SettingsCategory::Graphics)) {
        const GraphicsSettings& g = m_user.graphics;
        applyQuality(g.quality, g.groups);
        cvars::vsync().set(g.vsync, CVarSource::Config);
        cvars::windowMode().set(int(g.windowMode), CVarSource::Config);
        cvars::resolutionX().set(g.resolution.x, CVarSource::Config);
        cvars::resolutionY().set(g.resolution.y, CVarSource::Config);
        cvars::monitor().set(g.monitor, CVarSource::Config);
        cvars::maxFps().set(g.maxFps, CVarSource::Config);
        cvars::fov().set(g.fov, CVarSource::Config);
        setByName("r.RayTracing", g.rayTracing);
        setByName("r.Upscaler", g.upscaler);
        setByName("r.Upscaler.Quality", g.upscalerQuality);
    }
    if (hasCategory(categories, SettingsCategory::Audio)) {
        cvars::masterVolume().set(m_user.audio.masterVolume, CVarSource::Config);
    }
    if (hasCategory(categories, SettingsCategory::Gameplay) && !m_user.cvars.empty()) {
        nlohmann::json overrides = nlohmann::json::object();
        for (const auto& [k, v] : m_user.cvars) overrides[k] = v;
        CVarRegistry::instance().loadOverrides(overrides);
    }
    changed.emit(categories);
}

void Settings::setGraphics(const GraphicsSettings& graphics) {
    m_user.graphics = graphics;
    apply(SettingsCategory::Graphics);
}

void Settings::setAudio(const AudioSettings& audio) {
    m_user.audio = audio;
    apply(SettingsCategory::Audio);
}

void Settings::setInputRebinds(const std::map<std::string, std::string>& rebinds) {
    m_user.inputRebinds = rebinds;
    changed.emit(SettingsCategory::Input);
}

void Settings::captureFromCVars() {
    GraphicsSettings& g = m_user.graphics;
    g.vsync = cvars::vsync().get();
    g.windowMode = static_cast<WindowMode>(cvars::windowMode().get());
    g.resolution = {cvars::resolutionX().get(), cvars::resolutionY().get()};
    g.monitor = cvars::monitor().get();
    g.maxFps = cvars::maxFps().get();
    g.fov = cvars::fov().get();
    m_user.audio.masterVolume = cvars::masterVolume().get();
    auto& reg = CVarRegistry::instance();
    if (auto* c = reg.find("r.RayTracing")) g.rayTracing = c->toString() == "true";
    if (auto* c = reg.find("r.Upscaler")) g.upscaler = c->toString();
    if (auto* c = reg.find("r.Upscaler.Quality")) g.upscalerQuality = c->toString();

    const QualityLevel overall = scalability::overallLevel();
    g.groups.clear();
    if (overall != QualityLevel::Custom) {
        g.quality = std::string(scalability::levelName(overall));
    } else {
        g.quality = "Custom";
        for (usize i = 0; i < kScalabilityGroupCount; ++i) {
            const auto group = static_cast<Scalability>(i);
            // Custom groups are reproduced by their base level (sg.<Group>) + the persisted member cvars.
            QualityLevel level = scalability::currentLevel(group);
            if (level == QualityLevel::Custom) {
                const std::string sgName = "sg." + std::string(scalability::groupName(group));
                if (auto* sg = CVarRegistry::instance().findAs<int>(sgName)) level = static_cast<QualityLevel>(sg->get());
            }
            if (level != QualityLevel::Custom) {
                g.groups[std::string(scalability::groupName(group))] = std::string(scalability::levelName(level));
            }
        }
    }
    // Members of Custom scalability groups that differ from their group level (persisted even without the
    // Persist flag, like UE's per-cvar scalability overrides).
    std::set<std::string> customOverrides;
    const auto preset = scalability::savePreset();
    if (auto it = preset.find("overrides"); it != preset.end() && it->is_object()) {
        for (const auto& [name, value] : it->items()) {
            if (ICVar* c = reg.find(name)) {
                m_user.cvars[name] = c->toString();
                customOverrides.insert(name);
            }
        }
    }
    for (ICVar* c : reg.all()) {
        if (customOverrides.contains(c->name())) continue;
        if (c->group() != Scalability::None && !c->hasFlag(CVarFlags::Persist)) {
            m_user.cvars.erase(c->name()); // back at its group level
            continue;
        }
        if (!c->hasFlag(CVarFlags::Persist) || isManaged(c->name())) continue;
        if (c->isDefault()) {
            m_user.cvars.erase(c->name());
        } else {
            m_user.cvars[c->name()] = c->toString();
        }
    }
}

f32 Settings::busVolume(std::string_view bus) const {
    f32 v = 1.0f;
    if (auto it = m_user.audio.busVolumes.find(std::string(bus)); it != m_user.audio.busVolumes.end()) {
        v = it->second;
    } else if (auto p = m_project.audio.busVolumes.find(std::string(bus)); p != m_project.audio.busVolumes.end()) {
        v = p->second;
    }
    return v;
}

} // namespace ox
