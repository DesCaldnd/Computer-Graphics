#pragma once

#include "viewport/gizmo.hpp"

#include <QLabel>
#include <QMainWindow>
#include <QPointer>

class QActionGroup;
class QDockWidget;
class QProgressBar;
class QToolButton;

namespace ox::editor {

class ConsolePanel;
class ContentBrowserPanel;
class EditorContext;
class InspectorPanel;
class OutlinerPanel;
class ScalabilityWidget;
class StatsPanel;
class ViewportPanel;

// The editor shell: docks, menus, toolbar, status bar, scene/project commands, play-in-editor controls.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(EditorContext* ctx, QWidget* parent = nullptr);
    ~MainWindow() override;

    [[nodiscard]] ViewportPanel* viewport() const { return m_viewport; }
    [[nodiscard]] OutlinerPanel* outliner() const { return m_outliner; }
    [[nodiscard]] InspectorPanel* inspector() const { return m_inspector; }
    [[nodiscard]] ContentBrowserPanel* contentBrowser() const { return m_content; }
    [[nodiscard]] ConsolePanel* console() const { return m_console; }
    [[nodiscard]] StatsPanel* stats() const { return m_stats; }
    [[nodiscard]] QDockWidget* dock(const QString& name) const;

    void resetLayout();
    void saveLayout();
    void restoreLayout();
    bool maybeSave();
    void openProjectSettings(const QString& page = {});
    void openPreferences(const QString& page = {});
    void updateTitle();

Q_SIGNALS:
    // The language changed: the application rebuilds the window so every string is re-translated.
    void rebuildRequested();
    void switchProjectRequested();

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    void createDocks();
    void createActions();
    void createMenus();
    void createToolbar();
    void createStatusBar();
    QDockWidget* makeDock(const QString& objectName, const QString& title, const QString& icon, QWidget* content);
    void newScene();
    void openScene(const QString& path = {});
    bool saveScene(bool saveAs, bool json = false);
    void updatePlayActions();
    void saveThumbnail();
    void updateDockTitles();

    EditorContext* m_ctx;
    ViewportPanel* m_viewport = nullptr;
    OutlinerPanel* m_outliner = nullptr;
    InspectorPanel* m_inspector = nullptr;
    ContentBrowserPanel* m_content = nullptr;
    ConsolePanel* m_console = nullptr;
    StatsPanel* m_stats = nullptr;
    ScalabilityWidget* m_scalability = nullptr;
    QList<QDockWidget*> m_docks;
    QByteArray m_defaultState;
    // status bar
    QLabel* m_fpsLabel = nullptr;
    QLabel* m_gpuLabel = nullptr;
    QLabel* m_selLabel = nullptr;
    QLabel* m_modeLabel = nullptr;
    QProgressBar* m_progress = nullptr;
    // actions
    QAction* m_play = nullptr;
    QAction* m_simulate = nullptr;
    QAction* m_pause = nullptr;
    QAction* m_step = nullptr;
    QAction* m_stop = nullptr;
    QActionGroup* m_toolGroup = nullptr;
    QAction* m_localSpace = nullptr;
    QAction* m_snap = nullptr;
    QToolButton* m_qualityButton = nullptr;
    QString m_lastLanguage;
};

} // namespace ox::editor
