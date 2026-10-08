#pragma once

#include "theme/theme.hpp"

#include <QColor>
#include <QDateTime>
#include <QJsonObject>
#include <QKeySequence>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>

namespace ox::editor {

struct RecentProject {
    QString path; // .oxproject file
    QString name;
    QDateTime lastOpened;
};

// Per-user editor settings, stored as JSON in the user data dir (~/Library/Application Support/OxwaldEditor
// on macOS). Project-independent; project settings live in the project (see Project).
struct PreferenceValues {
    // appearance
    ThemeMode theme = ThemeMode::Dark;
    QColor accent = Theme::defaultAccent();
    double uiScale = 1.0; // applied on restart (QT_SCALE_FACTOR)
    int fontSize = 13;
    QString language = QStringLiteral("en"); // "en" | "ru"
    bool showSplash = true;
    // viewport
    double cameraSpeed = 5.0; // m/s
    double cameraAcceleration = 4.0;
    double cameraFov = 60.0;
    bool invertY = false;
    double mouseSensitivity = 0.25; // degrees per pixel
    double gridSize = 1.0;
    QColor gridColor = QColor(255, 255, 255, 40);
    double gizmoSize = 1.0;
    QColor selectionColor = QColor(255, 158, 26);
    QString viewportBackend = QStringLiteral("Auto"); // Auto | Software | Vulkan
    // snapping
    double translateSnap = 0.25;
    double rotateSnap = 15.0;
    double scaleSnap = 0.1;
    // autosave
    bool autosaveEnabled = true;
    int autosaveMinutes = 5;
    int autosaveBackups = 5;
    // source control / tools
    QString sourceControl = QStringLiteral("None"); // None | Git | Perforce
    QString codeEditorPath;
    QString codeEditorArgs = QStringLiteral("\"%f\":%l");
    // performance
    int frameLimit = 120; // 0 = unlimited
    bool throttleWhenUnfocused = true;
    int unfocusedFps = 10;
    // shortcuts (action id -> sequence), only overrides of the defaults
    QMap<QString, QKeySequence> shortcuts;
    QList<RecentProject> recentProjects;

    [[nodiscard]] QJsonObject toJson() const;
    void fromJson(const QJsonObject& o);
    bool operator==(const PreferenceValues&) const;
};

class EditorPreferences : public QObject {
    Q_OBJECT
public:
    explicit EditorPreferences(QObject* parent = nullptr);

    [[nodiscard]] const PreferenceValues& values() const { return m_values; }
    void setValues(const PreferenceValues& v);
    // Mutate + notify: prefs.modify([](auto& v) { v.cameraSpeed = 3; });
    template <class Fn>
    void modify(Fn&& fn) {
        PreferenceValues v = m_values;
        fn(v);
        setValues(v);
    }

    void addRecentProject(const QString& path, const QString& name);
    void removeRecentProject(const QString& path);

    // Storage directory (tests point it to a temp dir). File: <dir>/EditorPreferences.json
    void setStorageDir(const QString& dir) { m_dir = dir; }
    [[nodiscard]] QString storageDir() const { return m_dir; }
    [[nodiscard]] QString filePath() const;
    bool load();
    bool save() const;
    void resetToDefaults();

    // Applies theme/language to the running application.
    void applyAppearance() const;

Q_SIGNALS:
    void changed();

private:
    PreferenceValues m_values;
    QString m_dir;
};

} // namespace ox::editor
