#include "core/preferences.hpp"

#include "core/common.hpp"
#include "i18n/translator.hpp"

#include <oxwald/core/paths.hpp>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

namespace ox::editor {

namespace {
QString colorStr(const QColor& c) { return c.name(QColor::HexArgb); }
} // namespace

QJsonObject PreferenceValues::toJson() const {
    QJsonObject o;
    QJsonObject app;
    app["theme"] = theme == ThemeMode::Dark ? "Dark" : "Light";
    app["accent"] = colorStr(accent);
    app["uiScale"] = uiScale;
    app["fontSize"] = fontSize;
    app["language"] = language;
    app["showSplash"] = showSplash;
    o["appearance"] = app;
    QJsonObject vp;
    vp["cameraSpeed"] = cameraSpeed;
    vp["cameraAcceleration"] = cameraAcceleration;
    vp["cameraFov"] = cameraFov;
    vp["invertY"] = invertY;
    vp["mouseSensitivity"] = mouseSensitivity;
    vp["gridSize"] = gridSize;
    vp["gridColor"] = colorStr(gridColor);
    vp["gizmoSize"] = gizmoSize;
    vp["selectionColor"] = colorStr(selectionColor);
    vp["backend"] = viewportBackend;
    vp["translateSnap"] = translateSnap;
    vp["rotateSnap"] = rotateSnap;
    vp["scaleSnap"] = scaleSnap;
    o["viewport"] = vp;
    QJsonObject as;
    as["enabled"] = autosaveEnabled;
    as["intervalMinutes"] = autosaveMinutes;
    as["backups"] = autosaveBackups;
    o["autosave"] = as;
    QJsonObject sc;
    sc["provider"] = sourceControl;
    sc["codeEditor"] = codeEditorPath;
    sc["codeEditorArgs"] = codeEditorArgs;
    o["sourceControl"] = sc;
    QJsonObject perf;
    perf["frameLimit"] = frameLimit;
    perf["throttleWhenUnfocused"] = throttleWhenUnfocused;
    perf["unfocusedFps"] = unfocusedFps;
    o["performance"] = perf;
    QJsonObject keys;
    for (auto it = shortcuts.cbegin(); it != shortcuts.cend(); ++it) keys[it.key()] = it.value().toString(QKeySequence::PortableText);
    o["shortcuts"] = keys;
    QJsonArray recent;
    for (const auto& r : recentProjects) {
        QJsonObject e;
        e["path"] = r.path;
        e["name"] = r.name;
        e["lastOpened"] = r.lastOpened.toString(Qt::ISODate);
        recent.append(e);
    }
    o["recentProjects"] = recent;
    o["version"] = 1;
    return o;
}

void PreferenceValues::fromJson(const QJsonObject& o) {
    const PreferenceValues d;
    auto num = [](const QJsonObject& s, const char* k, double def) { return s.contains(k) ? s[k].toDouble(def) : def; };
    auto boolean = [](const QJsonObject& s, const char* k, bool def) { return s.contains(k) ? s[k].toBool(def) : def; };
    auto str = [](const QJsonObject& s, const char* k, const QString& def) { return s.contains(k) ? s[k].toString(def) : def; };
    auto col = [](const QJsonObject& s, const char* k, const QColor& def) {
        QColor c(s[k].toString());
        return c.isValid() ? c : def;
    };
    const QJsonObject app = o["appearance"].toObject();
    theme = str(app, "theme", "Dark") == QLatin1String("Light") ? ThemeMode::Light : ThemeMode::Dark;
    accent = col(app, "accent", d.accent);
    uiScale = std::clamp(num(app, "uiScale", d.uiScale), 0.5, 3.0);
    fontSize = std::clamp(int(num(app, "fontSize", d.fontSize)), 10, 20);
    language = str(app, "language", d.language);
    showSplash = boolean(app, "showSplash", d.showSplash);
    const QJsonObject vp = o["viewport"].toObject();
    cameraSpeed = num(vp, "cameraSpeed", d.cameraSpeed);
    cameraAcceleration = num(vp, "cameraAcceleration", d.cameraAcceleration);
    cameraFov = num(vp, "cameraFov", d.cameraFov);
    invertY = boolean(vp, "invertY", d.invertY);
    mouseSensitivity = num(vp, "mouseSensitivity", d.mouseSensitivity);
    gridSize = num(vp, "gridSize", d.gridSize);
    gridColor = col(vp, "gridColor", d.gridColor);
    gizmoSize = num(vp, "gizmoSize", d.gizmoSize);
    selectionColor = col(vp, "selectionColor", d.selectionColor);
    viewportBackend = str(vp, "backend", d.viewportBackend);
    translateSnap = num(vp, "translateSnap", d.translateSnap);
    rotateSnap = num(vp, "rotateSnap", d.rotateSnap);
    scaleSnap = num(vp, "scaleSnap", d.scaleSnap);
    const QJsonObject as = o["autosave"].toObject();
    autosaveEnabled = boolean(as, "enabled", d.autosaveEnabled);
    autosaveMinutes = int(num(as, "intervalMinutes", d.autosaveMinutes));
    autosaveBackups = int(num(as, "backups", d.autosaveBackups));
    const QJsonObject sc = o["sourceControl"].toObject();
    sourceControl = str(sc, "provider", d.sourceControl);
    codeEditorPath = str(sc, "codeEditor", d.codeEditorPath);
    codeEditorArgs = str(sc, "codeEditorArgs", d.codeEditorArgs);
    const QJsonObject perf = o["performance"].toObject();
    frameLimit = int(num(perf, "frameLimit", d.frameLimit));
    throttleWhenUnfocused = boolean(perf, "throttleWhenUnfocused", d.throttleWhenUnfocused);
    unfocusedFps = int(num(perf, "unfocusedFps", d.unfocusedFps));
    shortcuts.clear();
    const QJsonObject keys = o["shortcuts"].toObject();
    for (auto it = keys.begin(); it != keys.end(); ++it) {
        shortcuts.insert(it.key(), QKeySequence::fromString(it.value().toString(), QKeySequence::PortableText));
    }
    recentProjects.clear();
    for (const auto& v : o["recentProjects"].toArray()) {
        const QJsonObject e = v.toObject();
        recentProjects.push_back({e["path"].toString(), e["name"].toString(),
                                  QDateTime::fromString(e["lastOpened"].toString(), Qt::ISODate)});
    }
}

bool PreferenceValues::operator==(const PreferenceValues& o) const { return toJson() == o.toJson(); }

EditorPreferences::EditorPreferences(QObject* parent) : QObject(parent) {
    m_dir = qEnvironmentVariableIsSet("OX_EDITOR_PREFS_DIR") ? qEnvironmentVariable("OX_EDITOR_PREFS_DIR")
                                                              : qsPath(paths::userDataDir("OxwaldEditor"));
}

void EditorPreferences::setValues(const PreferenceValues& v) {
    if (v == m_values) return;
    m_values = v;
    Q_EMIT changed();
}

void EditorPreferences::addRecentProject(const QString& path, const QString& name) {
    modify([&](PreferenceValues& v) {
        v.recentProjects.erase(std::remove_if(v.recentProjects.begin(), v.recentProjects.end(),
                                              [&](const RecentProject& r) { return r.path == path; }),
                               v.recentProjects.end());
        v.recentProjects.prepend({path, name, QDateTime::currentDateTime()});
        while (v.recentProjects.size() > 12) v.recentProjects.removeLast();
    });
}

void EditorPreferences::removeRecentProject(const QString& path) {
    modify([&](PreferenceValues& v) {
        v.recentProjects.erase(std::remove_if(v.recentProjects.begin(), v.recentProjects.end(),
                                              [&](const RecentProject& r) { return r.path == path; }),
                               v.recentProjects.end());
    });
}

QString EditorPreferences::filePath() const { return QDir(m_dir).filePath(QStringLiteral("EditorPreferences.json")); }

bool EditorPreferences::load() {
    QFile f(filePath());
    if (!f.open(QIODevice::ReadOnly)) return false;
    const auto doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return false;
    PreferenceValues v;
    v.fromJson(doc.object());
    setValues(v);
    return true;
}

bool EditorPreferences::save() const {
    QDir().mkpath(m_dir);
    QSaveFile f(filePath());
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(m_values.toJson()).toJson(QJsonDocument::Indented));
    return f.commit();
}

void EditorPreferences::resetToDefaults() {
    PreferenceValues d;
    d.recentProjects = m_values.recentProjects;
    setValues(d);
}

void EditorPreferences::applyAppearance() const {
    Translator::instance().setLanguage(m_values.language);
    Theme::instance().apply(m_values.theme, m_values.accent, m_values.fontSize);
}

} // namespace ox::editor
