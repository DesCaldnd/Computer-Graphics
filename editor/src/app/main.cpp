// OxwaldEditor entry point.
//   OxwaldEditor [--project <file.oxproject>] [--browser]
//   QT_QPA_PLATFORM=offscreen OxwaldEditor --screenshots <dir>    (renders docs screenshots and exits)
#include "core/editor_context.hpp"
#include "core/log_capture.hpp"
#include "dialogs/branding.hpp"
#include "shell/editor_app.hpp"
#include "shell/main_window.hpp"

#include <oxwald/core/log.hpp>

#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QIcon>
#include <QMessageBox>
#include <QScreen>
#include <QTimer>
#include "viewport/viewport_panel.hpp"

int main(int argc, char** argv) {
    using namespace ox::editor;
    applyUiScaleFromPreferences();
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("OxwaldEditor"));
    QApplication::setOrganizationName(QStringLiteral("OxwaldEngine"));
    QApplication::setApplicationVersion(QStringLiteral(OX_EDITOR_VERSION));

    QCommandLineParser cli;
    cli.setApplicationDescription(QStringLiteral("OxwaldEngine editor"));
    cli.addHelpOption();
    cli.addVersionOption();
    QCommandLineOption projectOpt(QStringLiteral("project"), QStringLiteral("Project file to open."), QStringLiteral("file"));
    QCommandLineOption browserOpt(QStringLiteral("browser"), QStringLiteral("Always show the project browser."));
    QCommandLineOption shotsOpt(QStringLiteral("screenshots"), QStringLiteral("Render editor screenshots into <dir> and exit."), QStringLiteral("dir"));
    QCommandLineOption smokeOpt(QStringLiteral("smoke-seconds"), QStringLiteral("Quit after <s> seconds and report the viewport backend (CI smoke test)."), QStringLiteral("s"));
    QCommandLineOption grabOpt(QStringLiteral("grab"), QStringLiteral("With --smoke-seconds: save a screen capture of the main window to <file>."), QStringLiteral("file"));
    cli.addOptions({projectOpt, browserOpt, shotsOpt, smokeOpt, grabOpt});
    cli.process(app);

    LogCapture::instance().install();
    EditorContext ctx;
    ctx.preferences().load();
    ctx.preferences().applyAppearance();
    QApplication::setWindowIcon(QIcon(logoPixmap(256)));

    if (cli.isSet(shotsOpt)) {
        const QStringList files = generateScreenshots(ctx, cli.value(shotsOpt));
        for (const auto& f : files) OX_LOG_INFO("editor", "wrote {}", f.toStdString());
        return files.isEmpty() ? 1 : 0;
    }

    std::unique_ptr<SplashScreen> splash;
    if (ctx.preferences().values().showSplash) {
        splash = std::make_unique<SplashScreen>();
        splash->show();
        splash->setMessage(QObject::tr("Initialising engine modules…"), 20);
    }

    EditorApp editor(&ctx);
    bool opened = false;
    QString err;
    if (cli.isSet(projectOpt)) {
        if (splash) splash->setMessage(QObject::tr("Loading project…"), 55);
        opened = editor.openProject(cli.value(projectOpt), &err);
        if (!opened) QMessageBox::warning(nullptr, QStringLiteral("OxwaldEditor"), err);
    } else if (!cli.isSet(browserOpt)) {
        const auto& recent = ctx.preferences().values().recentProjects;
        if (!recent.isEmpty() && QFileInfo::exists(recent.first().path)) {
            if (splash) splash->setMessage(QObject::tr("Loading %1…").arg(recent.first().name), 55);
            opened = editor.openProject(recent.first().path, &err);
        }
    }
    if (splash) {
        splash->setMessage(QObject::tr("Ready"), 100);
        splash->hide();
    }
    if (!opened && !editor.chooseProject()) return 0;
    editor.showMainWindow();
    if (cli.isSet(smokeOpt)) {
        QTimer::singleShot(int(cli.value(smokeOpt).toDouble() * 1000), &app, [&] {
            MainWindow* w = editor.window();
            OX_LOG_INFO("editor", "smoke: viewport backend = {}, fps = {:.1f}, frame = {:.2f} ms",
                        w->viewport()->usingVulkan() ? "Vulkan (rhi)" : "software", w->viewport()->fps(), w->viewport()->frameMs());
            if (cli.isSet(grabOpt)) {
                if (QScreen* screen = w->screen()) screen->grabWindow(0, w->x(), w->y(), w->width(), w->height()).save(cli.value(grabOpt));
            }
            w->close();
            QApplication::quit();
        });
    }
    return QApplication::exec();
}
