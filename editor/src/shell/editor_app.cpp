#include "shell/editor_app.hpp"

#include "core/editor_context.hpp"
#include "dialogs/project_browser.hpp"
#include "shell/main_window.hpp"

#include <oxwald/core/paths.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace ox::editor {

void applyUiScaleFromPreferences() {
    const QString file = QDir(QString::fromStdString(paths::userDataDir("OxwaldEditor").string())).filePath(QStringLiteral("EditorPreferences.json"));
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return;
    const double scale = QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("appearance")).toObject().value(QStringLiteral("uiScale")).toDouble(1.0);
    if (scale > 0.4 && std::abs(scale - 1.0) > 0.01 && qEnvironmentVariableIsEmpty("QT_SCALE_FACTOR")) {
        qputenv("QT_SCALE_FACTOR", QByteArray::number(scale));
    }
}

EditorApp::EditorApp(EditorContext* ctx) : QObject(ctx), m_ctx(ctx) {}

EditorApp::~EditorApp() = default;

bool EditorApp::openProject(const QString& projectFile, QString* error) {
    auto p = Project::open(projectFile, error);
    if (!p) return false;
    m_ctx->preferences().addRecentProject(p->projectFile(), p->name());
    m_ctx->preferences().save();
    m_ctx->setProject(std::move(p));
    openStartupScene();
    return true;
}

void EditorApp::openStartupScene() {
    Project* p = m_ctx->project();
    if (!p) return;
    // Editor startup map (path inside the asset dir), else the game's startup scene (project:// URI).
    QString rel = QString::fromStdString(p->setting("editor.maps.editorStartupMap", "").get<std::string>());
    if (rel.isEmpty()) rel = QString::fromStdString(p->setting("startupScene", "").get<std::string>());
    const QString path = rel.isEmpty() ? QString() : p->pathForUri(rel);
    if (!path.isEmpty() && QFileInfo::exists(path)) m_ctx->openScene(path);
    else m_ctx->newScene(true);
}

bool EditorApp::chooseProject() {
    ProjectBrowser browser(m_ctx, m_window.get());
    if (browser.exec() != QDialog::Accepted) return false;
    auto p = browser.takeProject();
    if (!p) return false;
    m_ctx->setProject(std::move(p));
    openStartupScene();
    return true;
}

void EditorApp::showMainWindow() {
    m_window = std::make_unique<MainWindow>(m_ctx);
    connect(m_window.get(), &MainWindow::rebuildRequested, this, &EditorApp::rebuildWindow, Qt::QueuedConnection);
    connect(m_window.get(), &MainWindow::switchProjectRequested, this, [this] {
        if (!m_window->maybeSave()) return;
        m_window->saveLayout();
        chooseProject();
    });
    m_window->show();
}

void EditorApp::rebuildWindow() {
    if (!m_window) return;
    m_window->saveLayout();
    const QByteArray geo = m_window->saveGeometry();
    m_window.reset();
    showMainWindow();
    m_window->restoreGeometry(geo);
}

} // namespace ox::editor
