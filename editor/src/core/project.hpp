#pragma once

#include <oxwald/core/result.hpp>

#include <QObject>
#include <QString>

#include <nlohmann/json.hpp>

#include <memory>

namespace ox::editor {

// An OxwaldEngine project on disk:
//   <root>/<Name>.oxproject            JSON: name, version, startupScene, engineVersion, ...
//   <root>/Content/                    assets, scenes, prefabs (content browser root)
//   <root>/Config/ProjectSettings.json project settings (sections + "cvars" overrides + "scalability" preset)
//   <root>/Saved/                      editor layout, autosaves/backups, thumbnail.png (recent projects list)
// When the runtime module lands its ProjectSettings reads the same Config/ProjectSettings.json.
class Project : public QObject {
    Q_OBJECT
public:
    // Creates the folder structure + project file. Template "Blank" or "Showcase" (sample scene).
    static std::unique_ptr<Project> create(const QString& parentDir, const QString& name, const QString& templ,
                                           QString* error);
    static std::unique_ptr<Project> open(const QString& projectFile, QString* error);
    // A throw-away project in a temp dir (tests, "no project" sessions).
    static std::unique_ptr<Project> createTemporary(const QString& name = QStringLiteral("Untitled"));

    [[nodiscard]] QString name() const { return m_name; }
    void setName(const QString& n) { m_name = n; }
    [[nodiscard]] QString rootDir() const { return m_root; }
    [[nodiscard]] QString projectFile() const { return m_file; }
    [[nodiscard]] QString contentDir() const;
    [[nodiscard]] QString configDir() const;
    [[nodiscard]] QString savedDir() const;
    [[nodiscard]] QString settingsFile() const;
    [[nodiscard]] QString thumbnailFile() const;

    // Settings JSON. Paths are "section.key" (e.g. "general.version", "physics.gravity").
    [[nodiscard]] const nlohmann::json& settings() const { return m_settings; }
    nlohmann::json& settings() { return m_settings; }
    [[nodiscard]] nlohmann::json setting(const std::string& path, const nlohmann::json& def = {}) const;
    void setSetting(const std::string& path, const nlohmann::json& value);

    // Pushes "cvars" and "scalability" from the settings into the CVar registry.
    void applyCVarSettings() const;
    // Captures the current persisted cvar overrides + scalability preset into the settings.
    void captureCVarSettings();

    Status save();
    Status reloadSettings();

Q_SIGNALS:
    void settingsChanged();

private:
    QString m_root;
    QString m_file;
    QString m_name;
    nlohmann::json m_settings = nlohmann::json::object();
};

[[nodiscard]] nlohmann::json defaultProjectSettings(const QString& name);

} // namespace ox::editor
