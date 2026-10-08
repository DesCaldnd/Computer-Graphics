#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>

namespace ox::editor {

class EditorContext;
class MainWindow;

// Application bootstrap: preferences/theme, project selection (browser or last project), main window lifetime
// (rebuilt on language change), project switching.
class EditorApp : public QObject {
    Q_OBJECT
public:
    explicit EditorApp(EditorContext* ctx);
    ~EditorApp() override;

    // Opens a project file and its startup scene. Returns false (and error) on failure.
    bool openProject(const QString& projectFile, QString* error = nullptr);
    // Shows the project browser; false when the user cancelled.
    bool chooseProject();
    void showMainWindow();
    [[nodiscard]] MainWindow* window() const { return m_window.get(); }

private:
    void rebuildWindow();
    void openStartupScene();

    EditorContext* m_ctx;
    std::unique_ptr<MainWindow> m_window;
};

// Reads the UI scale from the saved preferences before QApplication exists (QT_SCALE_FACTOR).
void applyUiScaleFromPreferences();

// Renders every main window/dialog offscreen into PNGs (docs/guide/images/editor). Returns the files written.
QStringList generateScreenshots(EditorContext& ctx, const QString& outputDir);

} // namespace ox::editor
