#include "shell/main_window.hpp"

#include "core/editor_context.hpp"
#include "core/log_capture.hpp"
#include "dialogs/branding.hpp"
#include "inspector/inspector_panel.hpp"
#include "dialogs/save_game_inspector.hpp"
#include "integration/gameplay_tools.hpp"
#include "panels/behavior_tree_panel.hpp"
#include "panels/console_panel.hpp"
#include "panels/coroutines_panel.hpp"
#include "panels/content_browser.hpp"
#include "panels/outliner_panel.hpp"
#include "panels/stats_panel.hpp"
#include "settings/preferences_dialog.hpp"
#include "settings/project_settings_dialog.hpp"
#include "settings/scalability_widget.hpp"
#include "theme/icons.hpp"
#include "theme/theme.hpp"
#include "viewport/viewport_panel.hpp"
#include "widgets/widgets.hpp"

#include <oxwald/core/scalability.hpp>
#include <oxwald/scene/prefab.hpp>

#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QProcess>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressBar>
#include <QSettings>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace ox::editor {

MainWindow::MainWindow(EditorContext* ctx, QWidget* parent) : QMainWindow(parent), m_ctx(ctx) {
    setObjectName(QStringLiteral("OxwaldMainWindow"));
    setWindowIcon(QIcon(logoPixmap(256)));
    setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowTabbedDocks | QMainWindow::AllowNestedDocks | QMainWindow::GroupedDragging);
    setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);
    setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
    setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);
    m_lastLanguage = ctx->preferences().values().language;

    m_viewport = new ViewportPanel(ctx, this);
    auto* central = new QWidget(this);
    auto* cl = new QVBoxLayout(central);
    cl->setContentsMargins(4, 4, 0, 0);
    auto* frame = new QFrame(central);
    frame->setProperty("role", "card");
    auto* fl = new QVBoxLayout(frame);
    fl->setContentsMargins(1, 1, 1, 1);
    fl->addWidget(m_viewport);
    cl->addWidget(frame);
    setCentralWidget(central);

    createDocks();
    createActions();
    createMenus();
    createToolbar();
    createStatusBar();
    m_defaultState = saveState();
    resize(1600, 960);
    restoreLayout();
    updateTitle();
    updatePlayActions();

    connect(ctx, &EditorContext::sceneChanged, this, &MainWindow::updateTitle);
    connect(ctx, &EditorContext::projectChanged, this, [this] {
        updateTitle();
        restoreLayout();
    });
    connect(ctx, &EditorContext::playStateChanged, this, &MainWindow::updatePlayActions);
    connect(ctx, &EditorContext::openSourceRequested, this, &MainWindow::openSource);
    ctx->play().setEditTicking(true);
    connect(ctx, &EditorContext::statusMessage, this, [this](const QString& t, int ms) { statusBar()->showMessage(t, ms); });
    connect(&ctx->preferences(), &EditorPreferences::changed, this, [this] {
        m_ctx->actions().applyOverrides(m_ctx->preferences().values().shortcuts);
        if (m_ctx->preferences().values().language != m_lastLanguage) {
            m_lastLanguage = m_ctx->preferences().values().language;
            QTimer::singleShot(0, this, &MainWindow::rebuildRequested);
        }
    });
    ctx->actions().applyOverrides(ctx->preferences().values().shortcuts);
}

MainWindow::~MainWindow() {
    m_ctx->play().setEditTicking(false);
    m_ctx->actions().clear();
}

void MainWindow::openSource(const QString& file, int line) {
    if (file.isEmpty()) return;
    const auto& prefs = m_ctx->preferences().values();
    if (!prefs.codeEditorPath.isEmpty()) {
        QStringList args = prefs.codeEditorArgs.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        for (auto& a : args) a.replace(QLatin1String("%f"), file).replace(QLatin1String("%l"), QString::number(std::max(1, line)));
        if (QProcess::startDetached(prefs.codeEditorPath, args)) return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(file));
}

QDockWidget* MainWindow::makeDock(const QString& objectName, const QString& title, const QString& icon, QWidget* content) {
    auto* d = new QDockWidget(title, this);
    d->setObjectName(objectName);
    d->setWidget(content);
    d->setTitleBarWidget(new DockTitleBar(title, icon, d));
    d->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    d->toggleViewAction()->setIcon(Icons::get(icon));
    m_docks.push_back(d);
    auto later = [this] { QTimer::singleShot(0, this, &MainWindow::updateDockTitles); };
    connect(d, &QDockWidget::dockLocationChanged, this, later);
    connect(d, &QDockWidget::topLevelChanged, this, later);
    connect(d, &QDockWidget::visibilityChanged, this, later);
    return d;
}

QDockWidget* MainWindow::dock(const QString& name) const {
    for (auto* d : m_docks) {
        if (d->objectName() == name) return d;
    }
    return nullptr;
}

void MainWindow::createDocks() {
    m_outliner = new OutlinerPanel(m_ctx, this);
    m_inspector = new InspectorPanel(m_ctx, this);
    m_content = new ContentBrowserPanel(m_ctx, this);
    m_console = new ConsolePanel(m_ctx, this);
    m_stats = new StatsPanel(m_ctx, m_viewport, this);
    m_coroutines = new CoroutinesPanel(m_ctx, this);
    m_behaviorTree = new BehaviorTreePanel(m_ctx, this);
    m_scalability = new ScalabilityWidget(m_ctx, true, this);
    auto* scalHost = new QWidget(this);
    auto* sl = new QVBoxLayout(scalHost);
    sl->setContentsMargins(8, 8, 8, 8);
    sl->addWidget(m_scalability);
    sl->addStretch(1);

    auto* dOut = makeDock(QStringLiteral("dock.outliner"), tr("Outliner"), QStringLiteral("outliner"), m_outliner);
    auto* dIns = makeDock(QStringLiteral("dock.inspector"), tr("Inspector"), QStringLiteral("inspector"), m_inspector);
    auto* dCon = makeDock(QStringLiteral("dock.content"), tr("Content Browser"), QStringLiteral("content"), m_content);
    auto* dLog = makeDock(QStringLiteral("dock.console"), tr("Console"), QStringLiteral("console"), m_console);
    auto* dSta = makeDock(QStringLiteral("dock.stats"), tr("Stats"), QStringLiteral("stats"), m_stats);
    auto* dSca = makeDock(QStringLiteral("dock.scalability"), tr("Scalability"), QStringLiteral("speedometer"), scalHost);
    auto* dCor = makeDock(QStringLiteral("dock.coroutines"), tr("Coroutines"), QStringLiteral("coroutine"), m_coroutines);
    auto* dBt = makeDock(QStringLiteral("dock.behaviorTree"), tr("Behavior Tree"), QStringLiteral("sitemap"), m_behaviorTree);
    addDockWidget(Qt::RightDockWidgetArea, dOut);
    splitDockWidget(dOut, dIns, Qt::Vertical);
    addDockWidget(Qt::BottomDockWidgetArea, dCon);
    tabifyDockWidget(dCon, dLog);
    tabifyDockWidget(dLog, dSta);
    tabifyDockWidget(dSta, dCor);
    tabifyDockWidget(dCor, dBt);
    dCon->raise();
    addDockWidget(Qt::RightDockWidgetArea, dSca);
    tabifyDockWidget(dIns, dSca);
    dIns->raise();
    dSca->hide();
    resizeDocks({dOut, dIns}, {300, 560}, Qt::Vertical);
    resizeDocks({dOut}, {380}, Qt::Horizontal);
    resizeDocks({dCon}, {300}, Qt::Vertical);
    connect(m_content, &ContentBrowserPanel::openSceneRequested, this, [this](const QString& p) { openScene(p); });
}

void MainWindow::createActions() {
    ActionRegistry& r = m_ctx->actions();
    auto make = [&](const QString& id, const QString& cat, const QString& icon, const QString& text, const QKeySequence& ks, auto fn) {
        auto* a = new QAction(icon.isEmpty() ? QIcon() : Icons::get(icon), text, this);
        r.add(id, cat, a, ks);
        a->setShortcutContext(Qt::WindowShortcut);
        connect(a, &QAction::triggered, this, fn);
        addAction(a);
        return a;
    };
    const QString F = tr("File"), E = tr("Edit"), T = tr("Tools"), P = tr("Play"), V = tr("Viewport");
    make("file.newProject", F, "folder-plus", tr("New Project…"), QKeySequence(), [this] { Q_EMIT switchProjectRequested(); });
    make("file.openProject", F, "open", tr("Open Project…"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O), [this] { Q_EMIT switchProjectRequested(); });
    make("file.newScene", F, "file-plus", tr("New Scene"), QKeySequence::New, [this] { newScene(); });
    make("file.openScene", F, "scene", tr("Open Scene…"), QKeySequence::Open, [this] { openScene(); });
    make("file.save", F, "save", tr("Save Scene"), QKeySequence::Save, [this] { saveScene(false); });
    make("file.saveAs", F, "save", tr("Save Scene As…"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S), [this] { saveScene(true); });
    make("file.saveJson", F, "code", tr("Save Scene as JSON…"), QKeySequence(), [this] { saveScene(true, true); });
    make("file.quit", F, "close", tr("Quit"), QKeySequence::Quit, [this] { close(); });

    auto* undo = m_ctx->undoStack().createUndoAction(this, tr("Undo"));
    undo->setIcon(Icons::get(QStringLiteral("undo")));
    r.add(QStringLiteral("edit.undo"), E, undo, QKeySequence::Undo);
    addAction(undo);
    auto* redo = m_ctx->undoStack().createRedoAction(this, tr("Redo"));
    redo->setIcon(Icons::get(QStringLiteral("redo")));
    r.add(QStringLiteral("edit.redo"), E, redo, QKeySequence::Redo);
    addAction(redo);
    make("edit.cut", E, "cut", tr("Cut"), QKeySequence::Cut, [this] { m_ctx->cut(m_ctx->selection().ids()); });
    make("edit.copy", E, "copy", tr("Copy"), QKeySequence::Copy, [this] { m_ctx->copy(m_ctx->selection().ids()); });
    make("edit.paste", E, "paste", tr("Paste"), QKeySequence::Paste, [this] { m_ctx->paste(); });
    make("edit.duplicate", E, "duplicate", tr("Duplicate"), QKeySequence(Qt::CTRL | Qt::Key_D), [this] { m_ctx->duplicateEntities(m_ctx->selection().ids()); });
    make("edit.delete", E, "trash", tr("Delete"), QKeySequence::Delete, [this] {
        if (m_content->isAncestorOf(QApplication::focusWidget())) m_content->deleteSelected();
        else m_ctx->deleteEntities(m_ctx->selection().ids());
    });
    make("edit.rename", E, "rename", tr("Rename"), QKeySequence(Qt::Key_F2), [this] { m_outliner->renameSelected(); });
    make("edit.selectAll", E, "", tr("Select All"), QKeySequence::SelectAll, [this] {
        UuidList all;
        World& w = m_ctx->world();
        w.forEachInHierarchy([&](entt::entity h) { all.push_back(w.wrap(h).uuid()); });
        m_ctx->selection().set(all);
    });
    make("edit.deselect", E, "", tr("Deselect All"), QKeySequence(), [this] { m_ctx->selection().clear(); });
    make("edit.preferences", E, "settings", tr("Editor Preferences…"), QKeySequence::Preferences, [this] { openPreferences(); });
    make("edit.projectSettings", E, "sliders", tr("Project Settings…"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Comma), [this] { openProjectSettings(); });

    make("play.play", P, "play", tr("Play"), QKeySequence(Qt::ALT | Qt::Key_P), [this] {
        if (m_ctx->isPlaying()) m_ctx->play().setPaused(false);
        else m_ctx->startPlay(PlayMode::Play);
    });
    make("play.simulate", P, "simulate", tr("Simulate"), QKeySequence(Qt::ALT | Qt::Key_S), [this] {
        if (!m_ctx->isPlaying()) m_ctx->startPlay(PlayMode::Simulate);
    });
    make("play.pause", P, "pause", tr("Pause"), QKeySequence(Qt::Key_Pause), [this] {
        m_ctx->play().setPaused(m_ctx->play().state() != PlaySession::State::Paused);
    });
    make("play.step", P, "step", tr("Advance One Frame"), QKeySequence(), [this] { m_ctx->play().step(); });
    make("play.stop", P, "stop", tr("Stop"), QKeySequence(Qt::Key_Escape | Qt::SHIFT), [this] { m_ctx->stopPlay(); });
    m_play = r.action("play.play");
    m_simulate = r.action("play.simulate");
    m_pause = r.action("play.pause");
    m_step = r.action("play.step");
    m_stop = r.action("play.stop");
    m_pause->setCheckable(true);

    m_toolGroup = new QActionGroup(this);
    auto tool = [&](const QString& id, const QString& icon, const QString& text, QKeySequence ks, GizmoMode mode) {
        QAction* a = make(id, V, icon, text, ks, [this, mode] { m_viewport->setGizmoMode(mode); });
        a->setCheckable(true);
        m_toolGroup->addAction(a);
        a->setChecked(m_viewport->gizmo().mode() == mode);
    };
    tool("viewport.select", "select", tr("Select (Q)"), QKeySequence(Qt::Key_Q), GizmoMode::Select);
    tool("viewport.translate", "translate", tr("Move (W)"), QKeySequence(Qt::Key_W), GizmoMode::Translate);
    tool("viewport.rotate", "rotate", tr("Rotate (E)"), QKeySequence(Qt::Key_E), GizmoMode::Rotate);
    tool("viewport.scale", "scale", tr("Scale (R)"), QKeySequence(Qt::Key_R), GizmoMode::Scale);
    connect(m_viewport, &ViewportPanel::gizmoModeChanged, this, [this](GizmoMode m) {
        const char* ids[] = {"viewport.select", "viewport.translate", "viewport.rotate", "viewport.scale"};
        if (QAction* a = m_ctx->actions().action(QString::fromLatin1(ids[int(m)]))) a->setChecked(true);
    });
    m_localSpace = make("viewport.space", V, "world", tr("World / Local Space"), QKeySequence(Qt::CTRL | Qt::Key_QuoteLeft), [this] {
        const bool local = m_viewport->gizmo().space() == GizmoSpace::World;
        m_viewport->setGizmoSpace(local ? GizmoSpace::Local : GizmoSpace::World);
        m_localSpace->setIcon(Icons::get(local ? QStringLiteral("local") : QStringLiteral("world")));
        m_localSpace->setToolTip(local ? tr("Local space (click for world)") : tr("World space (click for local)"));
    });
    m_snap = make("viewport.snap", V, "snap", tr("Snapping"), QKeySequence(), [this](bool on) { m_viewport->setSnapEnabled(on); });
    m_snap->setCheckable(true);
    make("viewport.focus", V, "focus", tr("Focus Selection"), QKeySequence(Qt::Key_F), [this] { m_viewport->focusSelection(); });
    make("viewport.stats", V, "stats", tr("Show Stats"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Period), [this](bool on) { m_viewport->setShowStats(on); })
        ->setCheckable(true);

    make("tools.autodetect", T, "wand", tr("Auto-Detect Quality"), QKeySequence(), [this] {
        const QString t = m_scalability->autoDetect();
        statusBar()->showMessage(t, 6000);
    });
    make("tools.console", T, "console", tr("Console"), QKeySequence(Qt::Key_QuoteLeft), [this] {
        if (auto* d = dock(QStringLiteral("dock.console"))) {
            d->show();
            d->raise();
        }
        m_console->input()->setFocus();
    });
    make("tools.saveGames", T, "save-game", tr("Save Game Inspector…"), QKeySequence(), [this] {
        SaveGameInspector dlg(m_ctx, this);
        dlg.exec();
    });
    make("tools.bakeNav", T, "map", tr("Bake Navigation Mesh"), QKeySequence(), [this] {
        const BakeResult r = bakeNavMesh(*m_ctx);
        statusBar()->showMessage(r.message, 5000);
    });
    make("window.coroutines", T, "coroutine", tr("Coroutines"), QKeySequence(), [this] {
        if (auto* d = dock(QStringLiteral("dock.coroutines"))) {
            d->show();
            d->raise();
        }
    });
    make("window.behaviorTree", T, "sitemap", tr("Behavior Tree Debugger"), QKeySequence(), [this] {
        if (auto* d = dock(QStringLiteral("dock.behaviorTree"))) {
            d->show();
            d->raise();
        }
    });
    make("tools.projectFolder", T, "external", tr("Show Project Folder"), QKeySequence(), [this] {
        if (m_ctx->project()) QDesktopServices::openUrl(QUrl::fromLocalFile(m_ctx->project()->rootDir()));
    });
    make("help.about", tr("Help"), "info", tr("About OxwaldEditor"), QKeySequence(), [this] {
        AboutDialog dlg(m_ctx, this);
        dlg.exec();
    });
    make("help.docs", tr("Help"), "help", tr("Documentation"), QKeySequence(Qt::Key_F1), [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QStringLiteral(OX_EDITOR_SOURCE_DIR "/../docs/dev/modules/editor.md")));
    });
}

void MainWindow::createMenus() {
    ActionRegistry& r = m_ctx->actions();
    auto* mb = menuBar();
    mb->setNativeMenuBar(false);
    QMenu* file = mb->addMenu(tr("&File"));
    for (const char* id : {"file.newProject", "file.openProject"}) file->addAction(r.action(QString::fromLatin1(id)));
    file->addSeparator();
    for (const char* id : {"file.newScene", "file.openScene", "file.save", "file.saveAs", "file.saveJson"}) file->addAction(r.action(QString::fromLatin1(id)));
    QMenu* recent = file->addMenu(Icons::get(QStringLiteral("history")), tr("Recent Projects"));
    connect(recent, &QMenu::aboutToShow, this, [this, recent] {
        recent->clear();
        for (const auto& rp : m_ctx->preferences().values().recentProjects) {
            recent->addAction(rp.name + QStringLiteral("  —  ") + QFileInfo(rp.path).absolutePath())->setEnabled(false);
        }
        recent->addSeparator();
        recent->addAction(tr("Project Browser…"), this, &MainWindow::switchProjectRequested);
    });
    file->addSeparator();
    file->addAction(r.action("file.quit"));

    QMenu* edit = mb->addMenu(tr("&Edit"));
    for (const char* id : {"edit.undo", "edit.redo"}) edit->addAction(r.action(QString::fromLatin1(id)));
    edit->addSeparator();
    for (const char* id : {"edit.cut", "edit.copy", "edit.paste", "edit.duplicate", "edit.delete", "edit.rename"}) edit->addAction(r.action(QString::fromLatin1(id)));
    edit->addSeparator();
    edit->addAction(r.action("edit.selectAll"));
    edit->addAction(r.action("edit.deselect"));
    edit->addSeparator();
    edit->addAction(r.action("edit.preferences"));
    edit->addAction(r.action("edit.projectSettings"));

    QMenu* view = mb->addMenu(tr("&View"));
    for (const char* id : {"viewport.select", "viewport.translate", "viewport.rotate", "viewport.scale"}) view->addAction(r.action(QString::fromLatin1(id)));
    view->addSeparator();
    view->addAction(r.action("viewport.space"));
    view->addAction(r.action("viewport.snap"));
    view->addAction(r.action("viewport.focus"));
    view->addAction(r.action("viewport.stats"));
    view->addSeparator();
    QMenu* theme = view->addMenu(Icons::get(QStringLiteral("palette")), tr("Theme"));
    theme->addAction(tr("Dark"), this, [this] {
        m_ctx->preferences().modify([](PreferenceValues& v) { v.theme = ThemeMode::Dark; });
        m_ctx->preferences().applyAppearance();
        m_ctx->preferences().save();
    });
    theme->addAction(tr("Light"), this, [this] {
        m_ctx->preferences().modify([](PreferenceValues& v) { v.theme = ThemeMode::Light; });
        m_ctx->preferences().applyAppearance();
        m_ctx->preferences().save();
    });

    QMenu* entity = mb->addMenu(tr("E&ntity"));
    connect(entity, &QMenu::aboutToShow, this, [this, entity] {
        entity->clear();
        const UuidList sel = m_ctx->selection().ids();
        auto* create = entity->addMenu(Icons::get(QStringLiteral("add")), tr("Create"));
        OutlinerPanel::populateCreateMenu(create, m_ctx, {});
        if (!sel.empty()) {
            auto* child = entity->addMenu(Icons::get(QStringLiteral("add")), tr("Create Child"));
            OutlinerPanel::populateCreateMenu(child, m_ctx, sel.back());
        }
        entity->addSeparator();
        entity->addAction(m_ctx->actions().action("edit.duplicate"));
        entity->addAction(m_ctx->actions().action("edit.delete"));
        entity->addAction(m_ctx->actions().action("viewport.focus"));
        entity->addSection(tr("Prefab"));
        auto* mk = entity->addAction(Icons::get(QStringLiteral("prefab")), tr("Create Prefab from Selection…"), this, [this, sel] {
            Entity e = m_ctx->world().find(sel.back());
            const QString dir = m_ctx->project() ? m_ctx->project()->contentDir() + QStringLiteral("/Prefabs/") : QString();
            const QString path = QFileDialog::getSaveFileName(this, tr("Create Prefab"), dir + qs(e.name()) + QStringLiteral(".oxprefab"),
                                                              tr("Prefab (*.oxprefab);;Prefab JSON (*.oxprefab.json)"));
            QString err;
            if (!path.isEmpty() && !m_ctx->createPrefab(sel.back(), path, &err)) QMessageBox::warning(this, tr("Create Prefab"), err);
        });
        mk->setEnabled(!sel.empty() && !m_ctx->isPlaying());
        const bool inst = !sel.empty() && m_ctx->world().find(sel.back()).has<PrefabInstanceComponent>();
        entity->addAction(Icons::get(QStringLiteral("export")), tr("Apply Changes to Prefab"), this, [this, sel] {
            QString err;
            if (!m_ctx->applyPrefab(sel.back(), &err)) QMessageBox::warning(this, tr("Apply Prefab"), err);
        })->setEnabled(inst);
        entity->addAction(Icons::get(QStringLiteral("history")), tr("Revert All Overrides"), this, [this, sel] { m_ctx->revertAllPrefabOverrides(sel.back()); })
            ->setEnabled(inst);
        entity->addAction(Icons::get(QStringLiteral("open")), tr("Instantiate Prefab…"), this, [this] {
            const QString dir = m_ctx->project() ? m_ctx->project()->contentDir() : QString();
            const QString path = QFileDialog::getOpenFileName(this, tr("Instantiate Prefab"), dir, tr("Prefabs (*.oxprefab *.oxprefab.json)"));
            if (!path.isEmpty()) m_ctx->instantiatePrefab(path);
        });
    });

    QMenu* tools = mb->addMenu(tr("&Tools"));
    tools->addAction(r.action("tools.console"));
    tools->addAction(r.action("tools.autodetect"));
    tools->addAction(r.action("tools.projectFolder"));
    tools->addSeparator();
    tools->addAction(r.action("tools.saveGames"));
    tools->addAction(r.action("window.coroutines"));
    tools->addAction(r.action("window.behaviorTree"));
    tools->addAction(r.action("tools.bakeNav"));
    connect(tools, &QMenu::aboutToShow, this, [this] {
        if (QAction* a = m_ctx->actions().action(QStringLiteral("tools.bakeNav"))) a->setEnabled(!m_ctx->isPlaying() && gameplayAvailable());
    });
    tools->addSeparator();
    for (const char* id : {"play.play", "play.simulate", "play.pause", "play.step", "play.stop"}) tools->addAction(r.action(QString::fromLatin1(id)));

    QMenu* window = mb->addMenu(tr("&Window"));
    for (auto* d : m_docks) window->addAction(d->toggleViewAction());
    window->addSeparator();
    window->addAction(Icons::get(QStringLiteral("layout")), tr("Reset Layout"), this, &MainWindow::resetLayout);
    window->addAction(Icons::get(QStringLiteral("save")), tr("Save Layout"), this, [this] {
        saveLayout();
        statusBar()->showMessage(tr("Layout saved for this project"), 2500);
    });

    QMenu* help = mb->addMenu(tr("&Help"));
    help->addAction(r.action("help.docs"));
    help->addAction(Icons::get(QStringLiteral("keyboard")), tr("Keyboard Shortcuts…"), this, [this] { openPreferences(QStringLiteral("shortcuts")); });
    help->addSeparator();
    help->addAction(r.action("help.about"));
}

void MainWindow::createToolbar() {
    ActionRegistry& r = m_ctx->actions();
    auto* tb = addToolBar(tr("Main"));
    tb->setObjectName(QStringLiteral("toolbar.main"));
    tb->setMovable(false);
    tb->setIconSize(QSize(18, 18));
    tb->addAction(r.action("file.save"));
    tb->addAction(r.action("edit.undo"));
    tb->addAction(r.action("edit.redo"));
    tb->addSeparator();
    for (const char* id : {"viewport.select", "viewport.translate", "viewport.rotate", "viewport.scale"}) tb->addAction(r.action(QString::fromLatin1(id)));
    tb->addSeparator();
    tb->addAction(m_localSpace);
    m_localSpace->setToolTip(tr("World space (click for local)"));
    tb->addAction(m_snap);
    if (auto* snapBtn = qobject_cast<QToolButton*>(tb->widgetForAction(m_snap))) {
        auto* menu = new QMenu(snapBtn);
        connect(menu, &QMenu::aboutToShow, this, [this, menu] {
            menu->clear();
            const auto& v = m_ctx->preferences().values();
            menu->addSection(tr("Move"));
            for (double s : {0.01, 0.1, 0.25, 0.5, 1.0, 5.0, 10.0}) {
                auto* a = menu->addAction(QStringLiteral("%1 m").arg(s), this, [this, s] { m_ctx->preferences().modify([s](PreferenceValues& p) { p.translateSnap = s; }); });
                a->setCheckable(true);
                a->setChecked(std::abs(v.translateSnap - s) < 1e-6);
            }
            menu->addSection(tr("Rotate"));
            for (double s : {1.0, 5.0, 10.0, 15.0, 45.0, 90.0}) {
                auto* a = menu->addAction(QStringLiteral("%1°").arg(s), this, [this, s] { m_ctx->preferences().modify([s](PreferenceValues& p) { p.rotateSnap = s; }); });
                a->setCheckable(true);
                a->setChecked(std::abs(v.rotateSnap - s) < 1e-6);
            }
            menu->addSection(tr("Scale"));
            for (double s : {0.05, 0.1, 0.25, 0.5}) {
                auto* a = menu->addAction(QStringLiteral("×%1").arg(s), this, [this, s] { m_ctx->preferences().modify([s](PreferenceValues& p) { p.scaleSnap = s; }); });
                a->setCheckable(true);
                a->setChecked(std::abs(v.scaleSnap - s) < 1e-6);
            }
        });
        snapBtn->setMenu(menu);
        snapBtn->setPopupMode(QToolButton::MenuButtonPopup);
    }
    // camera speed
    auto* speed = new QToolButton(tb);
    speed->setIcon(Icons::get(QStringLiteral("camera")));
    speed->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    speed->setPopupMode(QToolButton::InstantPopup);
    speed->setProperty("role", "text");
    speed->setToolTip(tr("Camera speed"));
    auto updateSpeed = [this, speed] {
        const double s = m_ctx->preferences().values().cameraSpeed;
        speed->setText(QStringLiteral("%1").arg(s, 0, 'f', s < 10 ? 1 : 0));
    };
    updateSpeed();
    connect(&m_ctx->preferences(), &EditorPreferences::changed, speed, updateSpeed);
    auto* speedMenu = new QMenu(speed);
    for (double s : {0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 50.0, 100.0}) {
        speedMenu->addAction(QStringLiteral("%1 m/s").arg(s), this, [this, s] { m_ctx->preferences().modify([s](PreferenceValues& p) { p.cameraSpeed = s; }); });
    }
    speed->setMenu(speedMenu);
    tb->addWidget(speed);

    // play controls, centred
    auto* spacerL = new QWidget(tb);
    spacerL->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tb->addWidget(spacerL);
    auto* playGroup = new QFrame(tb);
    auto stylePlay = [playGroup] {
        playGroup->setStyleSheet(QStringLiteral("QFrame{background:%1;border:1px solid %2;border-radius:8px;}").arg(cssColor(colors().bg2), cssColor(colors().border)));
    };
    stylePlay();
    connect(&Theme::instance(), &Theme::changed, playGroup, stylePlay);
    auto* pl = new QHBoxLayout(playGroup);
    pl->setContentsMargins(3, 2, 3, 2);
    pl->setSpacing(1);
    for (QAction* a : {m_play, m_simulate, m_pause, m_step, m_stop}) {
        auto* b = new QToolButton(playGroup);
        b->setDefaultAction(a);
        b->setIconSize(QSize(18, 18));
        b->setAutoRaise(true);
        if (a == m_play) {
            b->setProperty("role", "play");
            b->setIcon(Icons::get(QStringLiteral("play"), Icons::Tint::Success));
        }
        if (a == m_stop) b->setProperty("role", "stop");
        pl->addWidget(b);
    }
    tb->addWidget(playGroup);
    auto* spacerR = new QWidget(tb);
    spacerR->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tb->addWidget(spacerR);

    // quality quick menu
    m_qualityButton = new QToolButton(tb);
    m_qualityButton->setObjectName(QStringLiteral("QualityButton"));
    m_qualityButton->setIcon(Icons::get(QStringLiteral("speedometer")));
    m_qualityButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_qualityButton->setPopupMode(QToolButton::InstantPopup);
    m_qualityButton->setProperty("role", "text");
    m_qualityButton->setToolTip(tr("Engine scalability (quality preset)"));
    auto updateQuality = [this] { m_qualityButton->setText(tr("Quality: %1").arg(levelDisplayName(scalability::overallLevel()))); };
    updateQuality();
    connect(m_scalability, &ScalabilityWidget::levelsChanged, this, updateQuality);
    auto* qm = new QMenu(m_qualityButton);
    connect(qm, &QMenu::aboutToShow, this, [this, qm, updateQuality] {
        qm->clear();
        qm->addSection(tr("Overall"));
        const QualityLevel cur = scalability::overallLevel();
        for (int i = 0; i < 4; ++i) {
            auto* a = qm->addAction(levelDisplayName(QualityLevel(i)), this, [this, i, updateQuality] {
                scalability::setOverall(QualityLevel(i));
                m_scalability->refresh();
                updateQuality();
            });
            a->setCheckable(true);
            a->setChecked(int(cur) == i);
        }
        qm->addSeparator();
        qm->addAction(m_ctx->actions().action("tools.autodetect"));
        qm->addAction(Icons::get(QStringLiteral("sliders")), tr("Scalability Settings…"), this, [this] { openProjectSettings(QStringLiteral("scalability")); });
        qm->addAction(Icons::get(QStringLiteral("speedometer")), tr("Show Scalability Panel"), this, [this] {
            if (auto* d = dock(QStringLiteral("dock.scalability"))) {
                d->show();
                d->raise();
            }
        });
    });
    m_qualityButton->setMenu(qm);
    tb->addWidget(m_qualityButton);
    tb->addAction(r.action("viewport.stats"));
    tb->addSeparator();
    tb->addAction(r.action("edit.projectSettings"));
    tb->addAction(r.action("edit.preferences"));
}

void MainWindow::createStatusBar() {
    auto* sb = statusBar();
    sb->setSizeGripEnabled(false);
    m_modeLabel = new QLabel(sb);
    m_selLabel = new QLabel(sb);
    m_gpuLabel = new QLabel(sb);
    m_fpsLabel = new QLabel(sb);
    m_progress = new QProgressBar(sb);
    m_progress->setFixedWidth(120);
    m_progress->setFixedHeight(6);
    m_progress->setVisible(false);
    sb->addPermanentWidget(m_progress);
    sb->addPermanentWidget(m_modeLabel);
    sb->addPermanentWidget(m_selLabel);
    sb->addPermanentWidget(m_gpuLabel);
    sb->addPermanentWidget(m_fpsLabel);
    const RenderingCaps caps = m_ctx->services().caps().caps();
    m_gpuLabel->setText(caps.gpuName + (caps.rayTracingSupported ? QStringLiteral("  ·  RT") : QString()));
    m_gpuLabel->setToolTip(tr("%1\nRay tracing: %2\nDLSS: %3")
                               .arg(caps.platform, caps.rayTracingSupported ? tr("supported") : caps.rayTracingUnavailableReason,
                                    caps.dlssSupported ? tr("supported") : caps.dlssUnavailableReason));
    auto updateSel = [this] {
        const auto n = m_ctx->selection().size();
        m_selLabel->setText(n == 0 ? tr("No selection") : tr("%n selected", nullptr, int(n)));
    };
    updateSel();
    connect(&m_ctx->selection(), &Selection::changed, this, updateSel);
    auto* t = new QTimer(this);
    connect(t, &QTimer::timeout, this, [this] {
        const RenderingCaps c = m_ctx->services().caps().caps();
        m_gpuLabel->setText(c.gpuName + (c.rayTracingSupported ? QStringLiteral("  ·  RT") : QString()));
        m_fpsLabel->setText(tr("%1 FPS · %2 ms").arg(m_viewport->fps(), 0, 'f', 0).arg(m_viewport->frameMs(), 0, 'f', 2));
    });
    t->start(500);
    sb->showMessage(tr("Ready"), 3000);
}

void MainWindow::updatePlayActions() {
    const bool active = m_ctx->isPlaying();
    const bool paused = m_ctx->play().state() == PlaySession::State::Paused;
    m_play->setEnabled(!active || paused);
    m_simulate->setEnabled(!active);
    m_pause->setEnabled(active);
    m_pause->setChecked(paused);
    m_step->setEnabled(true);
    m_stop->setEnabled(active);
    m_modeLabel->setText(!active ? tr("Edit mode") : paused ? tr("Paused") : m_ctx->play().mode() == PlayMode::Simulate ? tr("Simulating") : tr("Playing"));
    for (const char* id : {"file.save", "file.saveAs", "file.newScene", "file.openScene"}) {
        if (QAction* a = m_ctx->actions().action(QString::fromLatin1(id))) a->setEnabled(!active);
    }
}

void MainWindow::updateTitle() {
    const QString project = m_ctx->project() ? m_ctx->project()->name() : tr("No Project");
    setWindowTitle(QStringLiteral("%1%2 — %3 — OxwaldEditor").arg(m_ctx->sceneName(), m_ctx->isDirty() ? QStringLiteral("*") : QString(), project));
}

void MainWindow::updateDockTitles() {
    for (auto* d : m_docks) {
        bool tabbed = false;
        if (!d->isFloating()) {
            for (auto* o : tabifiedDockWidgets(d)) tabbed |= !o->isHidden();
        }
        if (auto* tb = qobject_cast<DockTitleBar*>(d->titleBarWidget())) tb->setCompact(tabbed);
    }
}

void MainWindow::resetLayout() {
    restoreState(m_defaultState);
    for (auto* d : m_docks) d->setFloating(false);
    if (auto* d = dock(QStringLiteral("dock.scalability"))) d->hide();
}

void MainWindow::saveLayout() {
    if (!m_ctx->project()) return;
    QSettings s(QDir(m_ctx->project()->savedDir()).filePath(QStringLiteral("EditorLayout.ini")), QSettings::IniFormat);
    s.setValue(QStringLiteral("geometry"), saveGeometry());
    s.setValue(QStringLiteral("state"), saveState(1));
}

void MainWindow::restoreLayout() {
    if (!m_ctx->project()) return;
    QSettings s(QDir(m_ctx->project()->savedDir()).filePath(QStringLiteral("EditorLayout.ini")), QSettings::IniFormat);
    if (s.contains(QStringLiteral("geometry"))) restoreGeometry(s.value(QStringLiteral("geometry")).toByteArray());
    if (s.contains(QStringLiteral("state"))) restoreState(s.value(QStringLiteral("state")).toByteArray(), 1);
    QTimer::singleShot(0, this, &MainWindow::updateDockTitles);
}

bool MainWindow::maybeSave() {
    if (m_ctx->isPlaying()) m_ctx->stopPlay();
    if (!m_ctx->isDirty()) return true;
    const auto r = QMessageBox::question(this, tr("Unsaved Changes"), tr("Save changes to %1?").arg(m_ctx->sceneName()),
                                         QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (r == QMessageBox::Cancel) return false;
    if (r == QMessageBox::Save) return saveScene(false);
    return true;
}

void MainWindow::newScene() {
    if (!maybeSave()) return;
    m_ctx->newScene(true);
}

void MainWindow::openScene(const QString& path) {
    QString p = path;
    if (p.isEmpty()) {
        const QString dir = m_ctx->project() ? m_ctx->project()->contentDir() : QString();
        p = QFileDialog::getOpenFileName(this, tr("Open Scene"), dir, tr("Scenes (*.oxscene *.oxscene.json)"));
        if (p.isEmpty()) return;
    }
    if (!maybeSave()) return;
    QString err;
    if (!m_ctx->openScene(p, &err)) QMessageBox::warning(this, tr("Open Scene"), err);
}

bool MainWindow::saveScene(bool saveAs, bool json) {
    QString path = m_ctx->scenePath();
    if (saveAs || path.isEmpty()) {
        const QString dir = m_ctx->project() ? QDir(m_ctx->project()->contentDir()).filePath(QStringLiteral("Scenes")) : QString();
        const QString base = QDir(dir).filePath(m_ctx->sceneName() + (json ? QStringLiteral(".oxscene.json") : QStringLiteral(".oxscene")));
        path = QFileDialog::getSaveFileName(this, json ? tr("Save Scene as JSON") : tr("Save Scene"), base,
                                            json ? tr("Scene JSON (*.oxscene.json)") : tr("Scene (*.oxscene);;Scene JSON (*.oxscene.json)"));
        if (path.isEmpty()) return false;
        if (json && !path.endsWith(QLatin1String(".json"))) path += QStringLiteral(".json");
    }
    QString err;
    if (!m_ctx->saveScene(path, &err)) {
        QMessageBox::warning(this, tr("Save Scene"), err);
        return false;
    }
    saveThumbnail();
    return true;
}

void MainWindow::saveThumbnail() {
    if (!m_ctx->project()) return;
    const QImage img = m_viewport->renderToImage(QSize(480, 288));
    QDir().mkpath(m_ctx->project()->savedDir());
    img.save(m_ctx->project()->thumbnailFile());
}

void MainWindow::openProjectSettings(const QString& page) {
    if (!m_ctx->project()) return;
    ProjectSettingsDialog dlg(m_ctx, this);
    if (!page.isEmpty()) dlg.showPage(page);
    dlg.exec();
    m_scalability->refresh();
}

void MainWindow::openPreferences(const QString& page) {
    PreferencesDialog dlg(m_ctx, this);
    if (!page.isEmpty()) dlg.showPage(page);
    dlg.exec();
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (!maybeSave()) {
        e->ignore();
        return;
    }
    saveLayout();
    saveThumbnail();
    m_ctx->preferences().save();
    QMainWindow::closeEvent(e);
}

} // namespace ox::editor
