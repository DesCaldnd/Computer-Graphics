#include "core/project.hpp"

#include "core/scene_templates.hpp"

#include <oxwald/core/cvar.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/core/scalability.hpp>
#include <oxwald/scene/scene_serializer.hpp>
#include <oxwald/scene/world.hpp>

#if OX_EDITOR_HAS_RUNTIME
#include <oxwald/runtime/json_io.hpp>
#include <oxwald/runtime/project.hpp>
#include <oxwald/runtime/settings.hpp>
#endif

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include <set>

namespace ox::editor {

namespace {

using nlohmann::json;

json binding(const char* action, const char* source, json modifiers = json::array(), json triggers = json::array()) {
    json b = {{"action", action}, {"source", source}};
    if (!modifiers.empty()) b["modifiers"] = std::move(modifiers);
    if (!triggers.empty()) b["triggers"] = std::move(triggers);
    return b;
}

// Runtime InputMappingConfig (plain JSON) with a WASD + mouse + gamepad layout.
json defaultInput() {
    const json negate = {{"type", "Negate"}};
    const json swizzle = {{"type", "Swizzle"}};
    const json pressed = json::array({{{"type", "Pressed"}}});
    json actions = json::array({{{"name", "Move"}, {"type", "Axis2D"}, {"description", "Walk / strafe"}},
                                {{"name", "Look"}, {"type", "Axis2D"}, {"description", "Camera look"}},
                                {{"name", "Jump"}, {"type", "Bool"}},
                                {{"name", "Sprint"}, {"type", "Bool"}},
                                {{"name", "Interact"}, {"type", "Bool"}},
                                {{"name", "Fire"}, {"type", "Bool"}}});
    json bindings = json::array({binding("Move", "Key.D"),
                                 binding("Move", "Key.A", json::array({negate})),
                                 binding("Move", "Key.W", json::array({swizzle})),
                                 binding("Move", "Key.S", json::array({negate, swizzle})),
                                 binding("Move", "Gamepad.LeftStick", json::array({{{"type", "DeadZone"}, {"lower", 0.2}}})),
                                 binding("Look", "Mouse.XY"),
                                 binding("Look", "Gamepad.RightStick", json::array({{{"type", "DeadZone"}, {"lower", 0.15}}})),
                                 binding("Jump", "Key.Space", json::array(), pressed),
                                 binding("Jump", "Gamepad.A", json::array(), pressed),
                                 binding("Sprint", "Key.LeftShift"),
                                 binding("Interact", "Key.E", json::array(), pressed),
                                 binding("Fire", "Mouse.Left")});
    return {{"actions", actions},
            {"contexts", json::array({{{"name", "Default"}, {"priority", 0}, {"bindings", bindings}}})},
            {"activeContexts", json::array({"Default"})}};
}

json editorDefaults() {
    return {{"description", ""},
            {"maps", {{"editorStartupMap", ""}, {"gameDefaultMap", ""}, {"defaultGameMode", "Default"}}},
            {"physics",
             {{"layers", {"Default", "Static", "Dynamic", "Player", "Trigger", "Debris"}}, {"collisionMatrix", json::array()}}},
            {"network", {{"port", 7777}, {"maxClients", 16}, {"tickRate", 30}, {"interpolationDelayMs", 100}}},
            {"scripting", {{"hotReload", true}, {"sandbox", true}}},
            {"packaging", {{"buildConfiguration", "Shipping"}, {"includeDebugFiles", false}}}};
}

bool readJsonFile(const QString& path, json& out) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    out = json::parse(f.readAll().toStdString(), nullptr, false, true);
    return !out.is_discarded() && out.is_object();
}

std::string cvarValueString(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    return v.dump();
}

} // namespace

nlohmann::json defaultProjectSettings(const QString& name) {
    json s = json::object();
#if OX_EDITOR_HAS_RUNTIME
    registerProjectTypes();
    ProjectSettings ps;
    s = json::parse(ox::json::toPlain(ps).dump());
#endif
    // Explicit defaults keep the file readable/stable even without the runtime module.
    s["name"] = name.toStdString();
    if (!s.contains("version")) s["version"] = "0.1.0";
    if (!s.contains("company")) s["company"] = "";
    s["startupScene"] = "";
    s["assetDirs"] = json::array({"Assets"});
    if (!s.contains("modules") || !s["modules"].is_object()) s["modules"] = json::object();
    if (!s.contains("saveVersion")) s["saveVersion"] = 1;
    s["input"] = defaultInput();
    s["physics"] = {{"gravity", {0.0, -9.81, 0.0}}, {"fixedRate", 60}, {"maxSubsteps", 8}, {"maxBodies", 65536}};
    s["audio"] = {{"sampleRate", 48000},
                  {"maxVoices", 64},
                  {"busVolumes", {{"Music", 0.8}, {"SFX", 1.0}, {"Voice", 1.0}, {"UI", 0.9}, {"Ambience", 1.0}}}};
    s["rendering"] = {{"cvars", json::object()}, {"rayTracingIfSupported", false}, {"upscaler", "Off"}};
    s["defaultQuality"] = "High";
    s["scalability"] = json::object();
    s["packaging"] = {{"alwaysIncludeAssets", json::array()},
                      {"targetPlatforms", {"macos", "windows", "linux"}},
                      {"outputDir", "build/package"},
                      {"compress", true}};
    s["editor"] = editorDefaults();
    return s;
}

QString Project::assetDirName() const {
    const json dirs = setting("assetDirs");
    if (dirs.is_array() && !dirs.empty() && dirs[0].is_string() && !dirs[0].get<std::string>().empty()) {
        return QString::fromStdString(dirs[0].get<std::string>());
    }
    return QStringLiteral("Assets");
}

QString Project::contentDir() const { return QDir(m_root).filePath(assetDirName()); }
QString Project::savedDir() const { return QDir(m_root).filePath(QStringLiteral("Saved")); }
QString Project::thumbnailFile() const { return QDir(savedDir()).filePath(QStringLiteral("thumbnail.png")); }

QString Project::uriForPath(const QString& absolutePath) const {
    const QString rel = QDir(contentDir()).relativeFilePath(absolutePath);
    return QStringLiteral("project://%1/%2").arg(assetDirName(), rel);
}

QString Project::pathForUri(const QString& uriOrRelative) const {
    QString s = uriOrRelative;
    if (s.startsWith(QLatin1String("project://"))) {
        s = s.mid(10);
        return QDir(m_root).filePath(s);
    }
    if (QFileInfo(s).isAbsolute()) return s;
    return QDir(contentDir()).filePath(s);
}

std::unique_ptr<Project> Project::create(const QString& parentDir, const QString& name, const QString& templ, QString* error) {
    const QString root = QDir(parentDir).filePath(name);
    if (QFileInfo::exists(QDir(root).filePath(name + QLatin1String(kExtension)))) {
        if (error) *error = QObject::tr("A project named \"%1\" already exists in this folder.").arg(name);
        return nullptr;
    }
    QDir d;
    for (const char* sub : {"Assets", "Assets/Scenes", "Assets/Prefabs", "Assets/Materials", "Assets/Textures", "Assets/Meshes",
                            "Assets/Scripts", "Assets/Audio", "Assets/AI", "Saved"}) {
        if (!d.mkpath(QDir(root).filePath(QString::fromLatin1(sub)))) {
            if (error) *error = QObject::tr("Cannot create folder %1").arg(root);
            return nullptr;
        }
    }
    auto p = std::unique_ptr<Project>(new Project());
    p->m_root = QDir(root).absolutePath();
    p->m_file = QDir(p->m_root).filePath(name + QLatin1String(kExtension));
    p->m_name = name;
    p->m_settings = defaultProjectSettings(name);
    {
        World world;
        if (templ == QLatin1String("Showcase")) {
            populateShowcaseScene(world);
            decorateShowcase(world, p->contentDir());
        } else {
            populateDefaultScene(world);
        }
        const QString scene = QDir(p->contentDir()).filePath(QStringLiteral("Scenes/Main.oxscene"));
        (void)saveScene(world, scene.toStdString());
        p->setSetting("startupScene", p->uriForPath(scene).toStdString());
        p->setSetting("editor.maps.editorStartupMap", "Scenes/Main.oxscene");
        p->setSetting("editor.maps.gameDefaultMap", "Scenes/Main.oxscene");
    }
    {
        QFile ignore(QDir(p->m_root).filePath(QStringLiteral(".gitignore")));
        if (ignore.open(QIODevice::WriteOnly)) ignore.write(".oxcache/\nSaved/\nbuild/\n");
    }
    if (auto st = p->save(); !st.hasValue()) {
        if (error) *error = QString::fromStdString(st.error().message);
        return nullptr;
    }
    return p;
}

std::unique_ptr<Project> Project::convertLegacy(const QString& legacyFile, QString* error) {
    json old;
    if (!readJsonFile(legacyFile, old)) {
        if (error) *error = QObject::tr("%1 is not a valid project file").arg(legacyFile);
        return nullptr;
    }
    const QFileInfo fi(legacyFile);
    auto p = std::unique_ptr<Project>(new Project());
    p->m_root = fi.absolutePath();
    p->m_name = QString::fromStdString(old.value("name", fi.completeBaseName().toStdString()));
    p->m_file = QDir(p->m_root).filePath(p->m_name + QLatin1String(kExtension));
    p->m_settings = defaultProjectSettings(p->m_name);
    p->m_settings["assetDirs"] = json::array({"Content"});
    json legacy;
    if (readJsonFile(QDir(p->m_root).filePath(QStringLiteral("Config/ProjectSettings.json")), legacy)) {
        auto copy = [&](const char* from, const std::string& to) {
            const json* cur = &legacy;
            for (const auto& part : QString::fromLatin1(from).split(QLatin1Char('.'))) {
                if (!cur->is_object() || !cur->contains(part.toStdString())) return;
                cur = &(*cur)[part.toStdString()];
            }
            p->setSetting(to, *cur);
        };
        copy("general.version", "version");
        copy("general.company", "company");
        copy("general.description", "editor.description");
        copy("physics.gravity", "physics.gravity");
        copy("physics.fixedRate", "physics.fixedRate");
        copy("physics.maxSubsteps", "physics.maxSubsteps");
        copy("physics.layers", "editor.physics.layers");
        copy("physics.collisionMatrix", "editor.physics.collisionMatrix");
        copy("audio.sampleRate", "audio.sampleRate");
        copy("audio.maxVoices", "audio.maxVoices");
        copy("network", "editor.network");
        copy("maps", "editor.maps");
    }
    const std::string startup = old.value("startupScene", std::string());
    if (!startup.empty()) p->setSetting("startupScene", "project://Content/" + startup);
    if (auto st = p->save(); !st) {
        if (error) *error = QString::fromStdString(st.error().message);
        return nullptr;
    }
    OX_LOG_INFO("editor", "Converted legacy project {} to {}", legacyFile.toStdString(), p->m_file.toStdString());
    return p;
}

std::unique_ptr<Project> Project::open(const QString& projectFile, QString* error) {
    QString file = projectFile;
    if (QFileInfo(file).isDir()) {
        QDir dir(file);
        QStringList found = dir.entryList({QStringLiteral("*") + QLatin1String(kExtension)}, QDir::Files, QDir::Name);
        if (found.isEmpty()) found = dir.entryList({QStringLiteral("*") + QLatin1String(kLegacyExtension)}, QDir::Files, QDir::Name);
        if (found.isEmpty()) {
            if (error) *error = QObject::tr("No project file in %1").arg(file);
            return nullptr;
        }
        file = dir.filePath(found.first());
    }
    if (file.endsWith(QLatin1String(kLegacyExtension))) {
        // A converted project may already sit next to the legacy file.
        const QFileInfo fi(file);
        const QStringList converted = fi.dir().entryList({QStringLiteral("*") + QLatin1String(kExtension)}, QDir::Files, QDir::Name);
        if (converted.isEmpty()) return convertLegacy(file, error);
        file = fi.dir().filePath(converted.first());
    }
    auto p = std::unique_ptr<Project>(new Project());
    p->m_file = QFileInfo(file).absoluteFilePath();
    p->m_root = QFileInfo(file).absolutePath();
    p->m_name = QFileInfo(file).completeBaseName();
    if (auto st = p->readFile(); !st) {
        if (error) *error = QObject::tr("%1 is not a valid project file (%2)").arg(file, QString::fromStdString(st.error().message));
        return nullptr;
    }
    QDir().mkpath(p->contentDir());
    QDir().mkpath(p->savedDir());
    return p;
}

std::unique_ptr<Project> Project::createTemporary(const QString& name) {
    const QString base = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                             .filePath(QStringLiteral("oxwald-temp-projects/") + QUuid::createUuid().toString(QUuid::WithoutBraces));
    QDir().mkpath(base);
    QString err;
    auto p = create(base, name, QStringLiteral("Blank"), &err);
    return p;
}

nlohmann::json Project::setting(const std::string& path, const nlohmann::json& def) const {
    const json* cur = &m_settings;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t dot = path.find('.', start);
        const std::string key = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!cur->is_object() || !cur->contains(key)) return def;
        cur = &(*cur)[key];
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return *cur;
}

void Project::setSetting(const std::string& path, const nlohmann::json& value) {
    json* cur = &m_settings;
    size_t start = 0;
    while (true) {
        const size_t dot = path.find('.', start);
        const std::string key = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!cur->is_object()) *cur = json::object();
        if (dot == std::string::npos) {
            if (cur->contains(key) && (*cur)[key] == value) return;
            (*cur)[key] = value;
            break;
        }
        cur = &(*cur)[key];
        start = dot + 1;
    }
    if (path == "name" && value.is_string()) m_name = QString::fromStdString(value.get<std::string>());
    Q_EMIT settingsChanged();
}

#if OX_EDITOR_HAS_RUNTIME
ProjectSettings Project::runtimeSettings() const {
    registerProjectTypes();
    ProjectSettings ps;
    json copy = m_settings;
    copy.erase("editor");
    (void)ox::json::fromPlain(nlohmann::ordered_json::parse(copy.dump()), ps);
    return ps;
}
#endif

void Project::applyCVarSettings() const {
#if OX_EDITOR_HAS_RUNTIME
    Settings s(nullptr);
    s.setProject(runtimeSettings());
    s.applyProjectDefaults();
#else
    const std::string overall = setting("defaultQuality", "High").get<std::string>();
    if (auto level = scalability::levelFromName(overall); level && *level != QualityLevel::Custom) scalability::setOverall(*level);
    if (auto groups = setting("scalability"); groups.is_object()) {
        for (const auto& [g, l] : groups.items()) {
            auto group = scalability::groupFromName(g);
            auto level = l.is_string() ? scalability::levelFromName(l.get<std::string>()) : std::nullopt;
            if (group && level && *level != QualityLevel::Custom) scalability::setGroup(*group, *level);
        }
    }
    if (auto cv = setting("rendering.cvars"); cv.is_object()) {
        for (const auto& [name, value] : cv.items()) {
            if (ICVar* c = CVarRegistry::instance().find(name)) c->setFromString(cvarValueString(value), CVarSource::Config);
            else CVarRegistry::instance().loadOverrides(json{{name, value}});
        }
    }
#endif
}

void Project::captureCVarSettings() {
    // Scalability: overall level or per-group base levels; member cvars changed away from their group level are
    // stored with the other persisted cvar overrides.
    const json preset = scalability::savePreset();
    const QualityLevel overall = scalability::overallLevel();
    json groups = json::object();
    if (overall == QualityLevel::Custom && preset.contains("groups")) groups = preset["groups"];
    std::set<std::string> members;
    for (usize i = 0; i < kScalabilityGroupCount; ++i) {
        for (ICVar* c : scalability::cvars(Scalability(i))) members.insert(std::string(c->name()));
    }
    static const std::set<std::string> userOwned = {"r.VSync",    "r.WindowMode", "r.ResolutionX", "r.ResolutionY",
                                                    "r.Monitor",  "t.MaxFPS",     "g.FOV",         "a.MasterVolume"};
    json cvars = json::object();
    for (const auto& [name, value] : CVarRegistry::instance().saveOverrides(false).items()) {
        if (name.starts_with("sg.") || members.contains(name) || userOwned.contains(name)) continue;
        cvars[name] = cvarValueString(value);
    }
    if (preset.contains("overrides")) {
        for (const auto& [name, value] : preset["overrides"].items()) cvars[name] = cvarValueString(value);
    }
    m_settings["defaultQuality"] = overall == QualityLevel::Custom ? std::string("Custom") : std::string(scalability::levelName(overall));
    m_settings["scalability"] = groups;
    m_settings["rendering"]["cvars"] = cvars;
    Q_EMIT settingsChanged();
}

Status Project::save() {
    QDir().mkpath(m_root);
    m_settings["name"] = m_name.toStdString();
    std::string text;
#if OX_EDITOR_HAS_RUNTIME
    // Normalise through the runtime type so the file is exactly what Engine/OxwaldPlayer read.
    registerProjectTypes();
    json runtimePart = m_settings;
    const json editorPart = m_settings.value("editor", json::object());
    runtimePart.erase("editor");
    ProjectSettings ps;
    if (!ox::json::fromPlain(nlohmann::ordered_json::parse(runtimePart.dump()), ps)) return makeError("invalid project settings");
    nlohmann::ordered_json out = ox::json::toPlain(ps);
    out["editor"] = nlohmann::ordered_json::parse(editorPart.dump());
    text = out.dump(2) + "\n";
    m_settings = json::parse(out.dump());
#else
    text = m_settings.dump(2) + "\n";
#endif
    QSaveFile f(m_file);
    if (!f.open(QIODevice::WriteOnly)) return makeError("cannot write {}", m_file.toStdString());
    f.write(QByteArray::fromStdString(text));
    if (!f.commit()) return makeError("cannot write {}", m_file.toStdString());
    return {};
}

Status Project::readFile() {
    json j;
    if (!readJsonFile(m_file, j)) return makeError("invalid JSON");
    json merged = defaultProjectSettings(QString::fromStdString(j.value("name", m_name.toStdString())));
    // Missing keys keep their defaults; maps that represent a whole value are taken as they are.
    merged.merge_patch(j);
    for (const char* whole : {"assetDirs", "input", "scalability"}) {
        if (j.contains(whole)) merged[whole] = j[whole];
    }
    if (j.contains("rendering") && j["rendering"].contains("cvars")) merged["rendering"]["cvars"] = j["rendering"]["cvars"];
    m_settings = merged;
    m_name = QString::fromStdString(m_settings.value("name", m_name.toStdString()));
    return {};
}

Status Project::reloadSettings() {
    if (auto st = readFile(); !st) return st;
    Q_EMIT settingsChanged();
    return {};
}

} // namespace ox::editor
