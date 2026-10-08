#pragma once

#include <oxwald/core/result.hpp>

#include <QObject>
#include <QString>

#include <nlohmann/json.hpp>

#include <memory>

#if OX_EDITOR_HAS_RUNTIME
namespace ox {
struct ProjectSettings;
}
#endif

namespace ox::editor {

// An OxwaldEngine project on disk (the runtime's layout, see docs/dev/modules/runtime.md):
//   <root>/<Name>.oxproj   plain JSON of ox::ProjectSettings (name, version, startupScene, assetDirs, modules,
//                          input, physics, audio, rendering cvars, defaultQuality + scalability, packaging) plus an
//                          "editor" object with editor-only data (maps, network, scripting, collision layers, ...)
//                          that the runtime ignores
//   <root>/Assets/         asset database root (AssetRegistry: sources + .meta), content browser root
//   <root>/.oxcache/       imported artifacts (AssetRegistry)
//   <root>/Saved/          editor layout, autosaves, thumbnail.png
// Legacy editor projects (<Name>.oxproject + Content/ + Config/ProjectSettings.json) are converted on open.
class Project : public QObject {
    Q_OBJECT
public:
    static constexpr const char* kExtension = ".oxproj";
    static constexpr const char* kLegacyExtension = ".oxproject";

    // Creates the folder structure + project file. Template "Blank" or "Showcase" (sample scene).
    static std::unique_ptr<Project> create(const QString& parentDir, const QString& name, const QString& templ,
                                           QString* error);
    // A .oxproj (or legacy .oxproject) file, or a directory containing one.
    static std::unique_ptr<Project> open(const QString& projectFile, QString* error);
    // A throw-away project in a temp dir (tests, "no project" sessions).
    static std::unique_ptr<Project> createTemporary(const QString& name = QStringLiteral("Untitled"));

    [[nodiscard]] QString name() const { return m_name; }
    void setName(const QString& n) { m_name = n; }
    [[nodiscard]] QString rootDir() const { return m_root; }
    [[nodiscard]] QString projectFile() const { return m_file; }
    // Asset database root: <root>/<assetDirs[0]> (default "Assets").
    [[nodiscard]] QString contentDir() const;
    [[nodiscard]] QString assetDirName() const;
    [[nodiscard]] QString savedDir() const;
    // The settings live in the project file itself.
    [[nodiscard]] QString settingsFile() const { return m_file; }
    [[nodiscard]] QString thumbnailFile() const;

    // "project://<assetDir>/<rel>" <-> absolute paths under contentDir().
    [[nodiscard]] QString uriForPath(const QString& absolutePath) const;
    [[nodiscard]] QString pathForUri(const QString& uriOrRelative) const;

    // Settings JSON (.oxproj layout). Paths are dot separated ("physics.gravity", "editor.network.port").
    [[nodiscard]] const nlohmann::json& settings() const { return m_settings; }
    nlohmann::json& settings() { return m_settings; }
    [[nodiscard]] nlohmann::json setting(const std::string& path, const nlohmann::json& def = {}) const;
    void setSetting(const std::string& path, const nlohmann::json& value);

#if OX_EDITOR_HAS_RUNTIME
    // The runtime view of the settings (what Engine/OxwaldPlayer read from the .oxproj).
    [[nodiscard]] ProjectSettings runtimeSettings() const;
#endif

    // Pushes the project defaults (defaultQuality + scalability groups, rendering.cvars) into cvars/scalability
    // exactly like the runtime's Settings::applyProjectDefaults().
    void applyCVarSettings() const;
    // Captures the scalability levels + persisted cvar overrides into defaultQuality / scalability /
    // rendering.cvars.
    void captureCVarSettings();

    Status save();
    Status reloadSettings();

Q_SIGNALS:
    void settingsChanged();

private:
    Project() = default;
    Status readFile();
    static std::unique_ptr<Project> convertLegacy(const QString& legacyFile, QString* error);

    QString m_root;
    QString m_file;
    QString m_name;
    nlohmann::json m_settings = nlohmann::json::object();
};

// Default .oxproj JSON (runtime ProjectSettings defaults + editor section) for a new project.
[[nodiscard]] nlohmann::json defaultProjectSettings(const QString& name);

} // namespace ox::editor
