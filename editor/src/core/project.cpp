#include "core/project.hpp"

#include "core/scene_templates.hpp"

#include <oxwald/core/cvar.hpp>
#include <oxwald/core/scalability.hpp>
#include <oxwald/scene/scene_serializer.hpp>
#include <oxwald/scene/world.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

namespace ox::editor {

nlohmann::json defaultProjectSettings(const QString& name) {
    using nlohmann::json;
    json s = json::object();
    s["general"] = {{"name", name.toStdString()},
                    {"version", "0.1.0"},
                    {"company", ""},
                    {"description", ""},
                    {"startupScene", ""}};
    s["maps"] = {{"editorStartupMap", ""}, {"gameDefaultMap", ""}, {"defaultGameMode", "Default"}};
    s["physics"] = {{"gravity", {0.0, -9.81, 0.0}},
                    {"fixedRate", 60},
                    {"maxSubsteps", 8},
                    {"layers", {"Default", "Static", "Dynamic", "Player", "Trigger", "Debris"}},
                    {"collisionMatrix", json::array()}};
    s["audio"] = {{"buses", {{"Master", 1.0}, {"Music", 0.8}, {"SFX", 1.0}, {"Voice", 1.0}, {"UI", 0.9}}},
                  {"sampleRate", 48000},
                  {"maxVoices", 64}};
    s["input"] = {{"actions",
                   json::array({{{"name", "MoveForward"}, {"keys", {"W", "Up"}}},
                                {{"name", "MoveBackward"}, {"keys", {"S", "Down"}}},
                                {{"name", "MoveLeft"}, {"keys", {"A", "Left"}}},
                                {{"name", "MoveRight"}, {"keys", {"D", "Right"}}},
                                {{"name", "Jump"}, {"keys", {"Space"}}},
                                {{"name", "Interact"}, {"keys", {"E"}}}})}};
    s["network"] = {{"port", 7777}, {"maxClients", 16}, {"tickRate", 30}, {"interpolationDelayMs", 100}};
    s["scripting"] = {{"hotReload", true}, {"sandbox", true}, {"scriptRoots", {"Content/Scripts"}}};
    s["packaging"] = {{"buildConfiguration", "Shipping"},
                      {"outputDir", "Build"},
                      {"compressPak", true},
                      {"includeDebugFiles", false},
                      {"platforms", {"macOS"}}};
    s["cvars"] = json::object();
    s["scalability"] = json::object();
    return s;
}

QString Project::contentDir() const { return QDir(m_root).filePath(QStringLiteral("Content")); }
QString Project::configDir() const { return QDir(m_root).filePath(QStringLiteral("Config")); }
QString Project::savedDir() const { return QDir(m_root).filePath(QStringLiteral("Saved")); }
QString Project::settingsFile() const { return QDir(configDir()).filePath(QStringLiteral("ProjectSettings.json")); }
QString Project::thumbnailFile() const { return QDir(savedDir()).filePath(QStringLiteral("thumbnail.png")); }

std::unique_ptr<Project> Project::create(const QString& parentDir, const QString& name, const QString& templ, QString* error) {
    const QString root = QDir(parentDir).filePath(name);
    if (QFileInfo::exists(QDir(root).filePath(name + QStringLiteral(".oxproject")))) {
        if (error) *error = QObject::tr("A project named \"%1\" already exists in this folder.").arg(name);
        return nullptr;
    }
    QDir d;
    for (const char* sub : {"Content", "Content/Scenes", "Content/Prefabs", "Content/Materials", "Content/Textures",
                            "Content/Meshes", "Content/Scripts", "Content/Audio", "Config", "Saved"}) {
        if (!d.mkpath(QDir(root).filePath(QString::fromLatin1(sub)))) {
            if (error) *error = QObject::tr("Cannot create folder %1").arg(root);
            return nullptr;
        }
    }
    auto p = std::unique_ptr<Project>(new Project());
    p->m_root = QDir(root).absolutePath();
    p->m_file = QDir(root).filePath(name + QStringLiteral(".oxproject"));
    p->m_name = name;
    p->m_settings = defaultProjectSettings(name);
    {
        World world;
        if (templ == QLatin1String("Showcase")) populateShowcaseScene(world);
        else populateDefaultScene(world);
        const QString scene = QDir(p->contentDir()).filePath(QStringLiteral("Scenes/Main.oxscene"));
        (void)saveScene(world, scene.toStdString());
        p->setSetting("general.startupScene", "Scenes/Main.oxscene");
        p->setSetting("maps.editorStartupMap", "Scenes/Main.oxscene");
        p->setSetting("maps.gameDefaultMap", "Scenes/Main.oxscene");
    }
    if (auto st = p->save(); !st.hasValue()) {
        if (error) *error = QString::fromStdString(st.error().message);
        return nullptr;
    }
    return p;
}

std::unique_ptr<Project> Project::open(const QString& projectFile, QString* error) {
    QFile f(projectFile);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QObject::tr("Cannot open %1").arg(projectFile);
        return nullptr;
    }
    auto j = nlohmann::json::parse(f.readAll().toStdString(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        if (error) *error = QObject::tr("%1 is not a valid project file").arg(projectFile);
        return nullptr;
    }
    auto p = std::unique_ptr<Project>(new Project());
    p->m_file = QFileInfo(projectFile).absoluteFilePath();
    p->m_root = QFileInfo(projectFile).absolutePath();
    p->m_name = QString::fromStdString(j.value("name", QFileInfo(projectFile).completeBaseName().toStdString()));
    p->m_settings = defaultProjectSettings(p->m_name);
    (void)p->reloadSettings();
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
    const nlohmann::json* cur = &m_settings;
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
    nlohmann::json* cur = &m_settings;
    size_t start = 0;
    while (true) {
        const size_t dot = path.find('.', start);
        const std::string key = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!cur->is_object()) *cur = nlohmann::json::object();
        if (dot == std::string::npos) {
            if ((*cur)[key] == value) return;
            (*cur)[key] = value;
            break;
        }
        cur = &(*cur)[key];
        start = dot + 1;
    }
    if (path == "general.name" && value.is_string()) m_name = QString::fromStdString(value.get<std::string>());
    Q_EMIT settingsChanged();
}

void Project::applyCVarSettings() const {
    if (auto it = m_settings.find("scalability"); it != m_settings.end() && it->is_object() && !it->empty()) {
        scalability::loadPreset(*it);
    } else {
        scalability::setOverall(QualityLevel::High); // project default until the user picks levels
    }
    if (auto it = m_settings.find("cvars"); it != m_settings.end() && it->is_object()) {
        CVarRegistry::instance().loadOverrides(*it);
    }
}

void Project::captureCVarSettings() {
    m_settings["cvars"] = CVarRegistry::instance().saveOverrides(false);
    m_settings["scalability"] = scalability::savePreset();
    Q_EMIT settingsChanged();
}

Status Project::save() {
    QDir().mkpath(configDir());
    nlohmann::json pj = {{"name", m_name.toStdString()},
                         {"version", setting("general.version", "0.1.0")},
                         {"engineVersion", OX_EDITOR_VERSION},
                         {"startupScene", setting("general.startupScene", "")},
                         {"fileVersion", 1}};
    {
        QSaveFile f(m_file);
        if (!f.open(QIODevice::WriteOnly)) return makeError("cannot write {}", m_file.toStdString());
        f.write(QByteArray::fromStdString(pj.dump(2)));
        if (!f.commit()) return makeError("cannot write {}", m_file.toStdString());
    }
    QSaveFile s(settingsFile());
    if (!s.open(QIODevice::WriteOnly)) return makeError("cannot write {}", settingsFile().toStdString());
    s.write(QByteArray::fromStdString(m_settings.dump(2)));
    if (!s.commit()) return makeError("cannot write {}", settingsFile().toStdString());
    return {};
}

Status Project::reloadSettings() {
    QFile f(settingsFile());
    if (!f.open(QIODevice::ReadOnly)) return makeError("no settings file");
    auto j = nlohmann::json::parse(f.readAll().toStdString(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) return makeError("invalid settings file");
    // merge over defaults so new sections appear in old projects
    nlohmann::json merged = defaultProjectSettings(m_name);
    merged.merge_patch(j);
    for (const char* arr : {"cvars", "scalability"}) {
        if (j.contains(arr)) merged[arr] = j[arr];
    }
    m_settings = merged;
    Q_EMIT settingsChanged();
    return {};
}

} // namespace ox::editor
