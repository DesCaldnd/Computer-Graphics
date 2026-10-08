#include "test_common.hpp"

#include "i18n/translator.hpp"
#include "panels/console_panel.hpp"
#include "settings/preferences_dialog.hpp"
#include "settings/project_settings_dialog.hpp"
#include "settings/render_cvars.hpp"
#include "settings/scalability_widget.hpp"
#include "shell/editor_app.hpp"
#include "theme/icons.hpp"
#include "theme/theme.hpp"
#include "widgets/widgets.hpp"

#include <oxwald/core/scalability.hpp>

#include <QComboBox>
#include <QPushButton>
#include <QStandardItemModel>

namespace ox::editor::test {

namespace {
QStringList g_warnings;
void captureMessages(QtMsgType type, const QMessageLogContext&, const QString& msg) {
    if (type != QtDebugMsg && type != QtInfoMsg) g_warnings << msg;
}
} // namespace

class SettingsTests : public QObject {
    Q_OBJECT
    std::unique_ptr<EditorContext> ctx;

    void useCaps(bool rt, const QString& reason) {
        RenderingCaps c = detectHostCaps();
        c.rayTracingSupported = rt;
        c.rayTracingUnavailableReason = rt ? QString() : reason;
        c.dlssSupported = false;
        c.dlssUnavailableReason = QStringLiteral("DLSS needs an NVIDIA RTX GPU (test)");
        c.upscalers = defaultUpscalers(c);
        ctx->services().setCapsProvider(std::make_unique<StaticCapsProvider>(c));
    }

private Q_SLOTS:
    void init() {
        ctx = makeContext();
        ctx->setProject(Project::createTemporary(QStringLiteral("SettingsTest")));
    }
    void cleanup() { ctx.reset(); }

    void scalabilityOverallLowSetsGroupCVars() {
        ProjectSettingsDialog dlg(ctx.get());
        ScalabilityWidget* sw = dlg.scalability();
        QVERIFY(sw);
        sw->overallControl()->button(0)->click(); // Low
        QCOMPARE(scalability::overallLevel(), QualityLevel::Low);
        QVERIFY(CVarRegistry::instance().findAs<int>("r.Shadows.CSM.Resolution"));
        QCOMPARE(CVarRegistry::instance().findAs<int>("r.Shadows.CSM.Resolution")->get(), 1024);
        QCOMPARE(CVarRegistry::instance().findAs<int>("r.Textures.Anisotropy")->get(), 2);
        QCOMPARE(sw->groupControl(Scalability::Shadows)->current(), 0);
        // group override -> overall becomes Custom
        sw->groupControl(Scalability::Shadows)->button(3)->click();
        QCOMPARE(CVarRegistry::instance().findAs<int>("r.Shadows.CSM.Resolution")->get(), 4096);
        QCOMPARE(scalability::overallLevel(), QualityLevel::Custom);
        QCOMPARE(sw->overallControl()->current(), -1);
        // changes are undoable inside the dialog and Revert restores the opening state
        dlg.undoStack().undo();
        QCOMPARE(CVarRegistry::instance().findAs<int>("r.Shadows.CSM.Resolution")->get(), 1024);
        QVERIFY(dlg.isDirty());
        dlg.revertButton()->click();
        QCOMPARE(scalability::overallLevel(), QualityLevel::High);
        // Apply writes the preset into the project settings
        sw->overallControl()->button(3)->click();
        dlg.applyButton()->click();
        QCOMPARE(ctx->project()->setting("defaultQuality", "").get<std::string>(), std::string("Ultra"));
        dlg.revertButton()->click();
        scalability::setOverall(QualityLevel::High);
    }

    void autoDetectAppliesLevels() {
        useCaps(false, QStringLiteral("no RT"));
        ScalabilityWidget sw(ctx.get(), true);
        const QString summary = sw.autoDetect();
        QVERIFY(summary.contains(QStringLiteral("CPU index")));
        QCOMPARE(scalability::currentLevel(Scalability::RayTracing), QualityLevel::Low);
        RenderingCaps strong;
        strong.discreteGpu = true;
        strong.vramBytes = 16ull << 30;
        strong.rayTracingSupported = true;
        strong.deviceAvailable = true;
        auto lv = HeuristicBenchmark::levelsFor(150, 150, strong);
        QCOMPARE(lv[usize(Scalability::Shadows)], QualityLevel::Ultra);
        auto weak = HeuristicBenchmark::levelsFor(15, 15, RenderingCaps{});
        QCOMPARE(weak[usize(Scalability::Shadows)], QualityLevel::Low);
        scalability::setOverall(QualityLevel::High);
    }

    void rayTracingToggleDisabledWhenUnsupported() {
        const QString reason = QStringLiteral("MoltenVK does not expose VK_KHR_ray_query on Apple GPUs");
        useCaps(false, reason);
        {
            ProjectSettingsDialog dlg(ctx.get());
            auto* rt = dlg.findChild<ToggleSwitch*>(QStringLiteral("setting:r.RayTracing"));
            QVERIFY(rt);
            QVERIFY(!rt->isEnabled());
            QVERIFY(rt->toolTip().contains(reason));
            auto* shadows = dlg.findChild<ToggleSwitch*>(QStringLiteral("setting:r.RayTracing.Shadows"));
            QVERIFY(shadows && !shadows->isEnabled());
            // DLSS entry of the upscaler combo is disabled with the reason
            auto* up = dlg.findChild<QComboBox*>(QStringLiteral("setting:r.Upscaler"));
            QVERIFY(up);
            auto* model = qobject_cast<QStandardItemModel*>(up->model());
            QVERIFY(!(model->item(2)->flags() & Qt::ItemIsEnabled));
            QVERIFY(model->item(2)->toolTip().contains(QStringLiteral("NVIDIA")));
            QVERIFY(model->item(1)->flags() & Qt::ItemIsEnabled); // FSR 1 always available
            QVERIFY(model->rowCount() >= 4);                      // TAAU (r.Upscaler = 3)
            QVERIFY(model->item(3)->flags() & Qt::ItemIsEnabled);
        }
        useCaps(true, {});
        {
            ProjectSettingsDialog dlg(ctx.get());
            auto* rt = dlg.findChild<ToggleSwitch*>(QStringLiteral("setting:r.RayTracing"));
            QVERIFY(rt && rt->isEnabled());
            rt->setChecked(true);
            QCOMPARE(CVarRegistry::instance().findAs<bool>(cvars::kRayTracing)->get(), true);
            auto* shadows = dlg.findChild<ToggleSwitch*>(QStringLiteral("setting:r.RayTracing.Shadows"));
            QVERIFY(shadows->isEnabled());
            dlg.revertButton()->click();
            QCOMPARE(CVarRegistry::instance().findAs<bool>(cvars::kRayTracing)->get(), false);
        }
    }

    void projectSettingsSearchAndBinding() {
        ProjectSettingsDialog dlg(ctx.get());
        dlg.setSearchText(QStringLiteral("gravity"));
        QVERIFY(dlg.page(QStringLiteral("physics"))->filter(QStringLiteral("gravity")) > 0);
        auto* port = dlg.findChild<NumberField*>(QStringLiteral("setting:project:editor.network.port"));
        QVERIFY(port);
        Q_EMIT port->edited(9000, EditPhase::Single);
        QCOMPARE(ctx->project()->setting("editor.network.port").get<int>(), 9000);
        dlg.applyButton()->click();
        QVERIFY(ctx->project()->reloadSettings());
        QCOMPARE(ctx->project()->setting("editor.network.port").get<int>(), 9000);
    }

    void preferencesPersist() {
        const QString dir = QDir(tempRoot()).filePath(QStringLiteral("prefs-persist"));
        QDir(dir).removeRecursively();
        {
            EditorPreferences p;
            p.setStorageDir(dir);
            p.modify([](PreferenceValues& v) {
                v.theme = ThemeMode::Light;
                v.accent = QColor("#16B88A");
                v.cameraSpeed = 12.5;
                v.language = QStringLiteral("ru");
                v.shortcuts.insert(QStringLiteral("file.save"), QKeySequence(QStringLiteral("Ctrl+Alt+S")));
            });
            p.addRecentProject(QStringLiteral("/tmp/x.oxproj"), QStringLiteral("X"));
            QVERIFY(p.save());
        }
        EditorPreferences q;
        q.setStorageDir(dir);
        QVERIFY(q.load());
        QCOMPARE(q.values().theme, ThemeMode::Light);
        QCOMPARE(q.values().accent, QColor("#16B88A"));
        QCOMPARE(q.values().cameraSpeed, 12.5);
        QCOMPARE(q.values().language, QStringLiteral("ru"));
        QCOMPARE(q.values().shortcuts.value(QStringLiteral("file.save")), QKeySequence(QStringLiteral("Ctrl+Alt+S")));
        QCOMPARE(q.values().recentProjects.size(), 1);
    }

    void preferencesDialogBindsAndReverts() {
        PreferencesDialog dlg(ctx.get());
        auto* speed = dlg.findChild<NumberField*>(QStringLiteral("setting:pref:viewport.cameraSpeed"));
        QVERIFY(speed);
        Q_EMIT speed->edited(33.0, EditPhase::Single);
        QCOMPARE(ctx->preferences().values().cameraSpeed, 33.0);
        QVERIFY(QFile::exists(ctx->preferences().filePath()));
        dlg.revertButton()->click();
        QCOMPARE(ctx->preferences().values().cameraSpeed, PreferenceValues{}.cameraSpeed);
    }

    void themeLoadsWithoutWarnings() {
        g_warnings.clear();
        auto prev = qInstallMessageHandler(captureMessages);
        Theme::instance().apply(ThemeMode::Dark, Theme::defaultAccent(), 13);
        QVERIFY2(Theme::instance().unresolvedTokens().isEmpty(), qPrintable(Theme::instance().unresolvedTokens().join(',')));
        // polish a few widgets so style sheet parse errors surface
        ProjectSettingsDialog dlg(ctx.get());
        dlg.show();
        pump(50);
        Theme::instance().apply(ThemeMode::Light, QColor("#E5A00D"), 14);
        pump(50);
        dlg.hide();
        Theme::instance().apply(ThemeMode::Dark, Theme::defaultAccent(), 13);
        qInstallMessageHandler(prev);
        for (const QString& w : g_warnings) QVERIFY2(!w.contains(QStringLiteral("style"), Qt::CaseInsensitive), qPrintable(w));
        // icons: every icon referenced exists
        for (const char* n : {"play", "pause", "stop", "translate", "rotate", "scale", "camera", "light", "mesh", "folder", "scene", "prefab",
                              "script", "material", "texture", "audio", "physics", "settings", "search", "eye", "lock", "add", "remove",
                              "undo", "redo", "save", "rtx", "speedometer", "wand"}) {
            QVERIFY2(Icons::exists(QString::fromLatin1(n)), n);
        }
        QVERIFY(Icons::names().size() > 100);
        QVERIFY(!Icons::pixmap(QStringLiteral("play"), 32, Qt::red).isNull());
    }

    void russianTranslation() {
        Translator::instance().setLanguage(QStringLiteral("ru"));
        QCOMPARE(QCoreApplication::translate("ox::editor::MainWindow", "&File"), QStringLiteral("&Файл"));
        QCOMPARE(QCoreApplication::translate("x", "Project Settings"), QStringLiteral("Настройки проекта"));
        QVERIFY(Translator::instance().entryCount() > 200);
        Translator::instance().setLanguage(QStringLiteral("en"));
        QCOMPARE(QCoreApplication::translate("x", "Project Settings"), QStringLiteral("Project Settings"));
    }

    void consoleExecutesCVars() {
        ConsolePanel console(ctx.get());
        console.execute(QStringLiteral("r.Shadows.CSM.Resolution 1024"));
        QCOMPARE(CVarRegistry::instance().findAs<int>("r.Shadows.CSM.Resolution")->get(), 1024);
        const QString out = console.execute(QStringLiteral("r.Shadows.CSM.Resolution"));
        QVERIFY(out.contains(QStringLiteral("1024")));
        QCOMPARE(console.history().size(), 2);
        console.execute(QStringLiteral("does.not.exist 1"));
        scalability::setOverall(QualityLevel::High);
    }

    void screenshotGenerationRuns() {
        const QString dir = QDir(tempRoot()).filePath(QStringLiteral("screenshots"));
        QDir(dir).removeRecursively();
        const QStringList files = generateScreenshots(*ctx, dir);
        QVERIFY2(files.size() >= 18, qPrintable(QString::number(files.size())));
        for (const QString& f : files) {
            QImage img(f);
            QVERIFY2(!img.isNull() && img.width() > 200, qPrintable(f));
        }
    }
};

OX_EDITOR_TEST(SettingsTests);

} // namespace ox::editor::test

#include "test_settings.moc"
