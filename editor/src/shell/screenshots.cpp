#include "core/editor_context.hpp"
#include "core/log_capture.hpp"
#include "dialogs/branding.hpp"
#include "dialogs/project_browser.hpp"
#include "dialogs/save_game_inspector.hpp"
#include "panels/behavior_tree_panel.hpp"
#include "panels/coroutines_panel.hpp"
#include "inspector/component_card.hpp"
#include "inspector/inspector_panel.hpp"
#include "panels/console_panel.hpp"
#include "panels/content_browser.hpp"
#include "settings/preferences_dialog.hpp"
#include "settings/project_settings_dialog.hpp"
#include "settings/scalability_widget.hpp"
#include "shell/editor_app.hpp"
#include "shell/main_window.hpp"
#include "theme/theme.hpp"
#include "viewport/viewport_panel.hpp"

#include <oxwald/core/log.hpp>
#include <oxwald/core/scalability.hpp>
#include <oxwald/scene/component_registry.hpp>
#include <oxwald/scene/runtime_id.hpp>

#if OX_EDITOR_HAS_ASYNC
#include <oxwald/async/scheduler.hpp>
#endif
#if OX_EDITOR_HAS_RUNTIME
#include <oxwald/runtime/engine.hpp>
#endif

#include <QTreeWidget>
#include <cmath>

#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QListView>
#include <QToolButton>

namespace ox::editor {

namespace {

void settle(int ms = 120) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        LogCapture::instance().flush();
    }
}

bool save(QWidget* w, const QString& dir, const QString& name, QStringList& out) {
    settle();
    const QPixmap pm = w->grab();
    const QString path = QDir(dir).filePath(name);
    if (!pm.save(path)) return false;
    out << path;
    return true;
}

Uuid findByName(World& w, const char* name) {
    Entity e = w.findByName(name);
    return e ? e.uuid() : Uuid{};
}

void frameCamera(ViewportPanel* vp) {
    ViewportCamera& c = vp->camera();
    c.position = {-9.5f, 5.6f, 11.0f};
    c.yaw = glm::radians(-40.0f);
    c.pitch = glm::radians(-19.0f);
    vp->requestRedraw();
}

} // namespace

QStringList generateScreenshots(EditorContext& ctx, const QString& outputDir) {
    QStringList out;
    QDir().mkpath(outputDir);
    const PreferenceValues savedPrefs = ctx.preferences().values();
    ctx.preferences().modify([](PreferenceValues& v) {
        v.theme = ThemeMode::Dark;
        v.accent = Theme::defaultAccent();
        v.language = QStringLiteral("en");
        v.fontSize = 13;
    });
    ctx.preferences().applyAppearance();

    // project with the showcase level + a prefab instance
    const QString base = QDir(QDir::tempPath()).filePath(QStringLiteral("oxwald-screenshots"));
    QDir(base).removeRecursively();
    QString err;
    auto project = Project::create(base, QStringLiteral("Showcase"), QStringLiteral("Showcase"), &err);
    if (!project) return out;
    const QString content = project->contentDir();
    ctx.setProject(std::move(project));
    ctx.openScene(QDir(content).filePath(QStringLiteral("Scenes/Main.oxscene")));
    if (Uuid crates = findByName(ctx.editWorld(), "Crates"); !crates.isNil()) {
        ctx.createPrefab(crates, QDir(content).filePath(QStringLiteral("Prefabs/CrateStack.oxprefab")));
        Entity c = ctx.editWorld().findByName("Crate B");
        if (c) ctx.setProperty({c.uuid()}, "Transform", "scale", serial::Value::makeVec3({0.9f, 0.6f, 0.9f}));
    }
    ctx.undoStack().setClean();
    {
        // Real, importable assets so the asset database shows types, thumbnails and import settings.
        auto writeFile = [&](const QString& rel, const QByteArray& data) {
            const QString path = QDir(content).filePath(rel);
            QDir().mkpath(QFileInfo(path).absolutePath());
            QFile file(path);
            if (file.open(QIODevice::WriteOnly)) file.write(data);
        };
        QImage tex(256, 256, QImage::Format_RGB32);
        for (int y = 0; y < 256; ++y)
            for (int x = 0; x < 256; ++x) tex.setPixel(x, y, ((x / 32 + y / 32) % 2) ? qRgb(186, 120, 70) : qRgb(120, 74, 40));
        tex.save(QDir(content).filePath(QStringLiteral("Textures/Checker_Wood.png")));
        tex.save(QDir(content).filePath(QStringLiteral("Checker_Wood.png")));
        QImage grad(256, 256, QImage::Format_RGB32);
        for (int y = 0; y < 256; ++y)
            for (int x = 0; x < 256; ++x) grad.setPixel(x, y, qRgb(40 + x / 3, 70 + y / 3, 160));
        grad.save(QDir(content).filePath(QStringLiteral("Textures/Sky_Gradient.png")));
        grad.save(QDir(content).filePath(QStringLiteral("Sky_Gradient.png")));
        QImage normal(128, 128, QImage::Format_RGB32);
        for (int y = 0; y < 128; ++y)
            for (int x = 0; x < 128; ++x) normal.setPixel(x, y, qRgb(128 + int(40 * std::sin(x * 0.2)), 128 + int(40 * std::sin(y * 0.2)), 235));
        normal.save(QDir(content).filePath(QStringLiteral("Textures/Bricks_normal.png")));
        writeFile(QStringLiteral("Materials/Brushed Steel.oxmat"),
                  R"({"oxmat":1,"shadingModel":"Lit","baseColor":[0.62,0.64,0.68,1],"metallic":1.0,"roughness":0.32})");
        writeFile(QStringLiteral("Materials/Painted Wood.oxmat"),
                  R"({"oxmat":1,"shadingModel":"Lit","baseColor":[0.55,0.32,0.18,1],"metallic":0.0,"roughness":0.7})");
        writeFile(QStringLiteral("Brushed Steel.oxmat"), R"({"oxmat":1,"baseColor":[0.62,0.64,0.68,1],"metallic":1.0,"roughness":0.32})");
        writeFile(QStringLiteral("Scripts/Door.lua"), "properties = { openAngle = { type = \"float\", default = 95, min = 0, max = 180 } }\nfunction onStart(self) end\n");
        writeFile(QStringLiteral("Door.lua"), "properties = { speed = { type = \"float\", default = 2 } }\n");
        writeFile(QStringLiteral("AI/Guard.oxbt"), R"({"root":{"type":"Sequence","children":[{"type":"Wait","seconds":1}]}})");
        writeFile(QStringLiteral("Water.frag"), "#version 460\nvoid main() {}\n");
        {
            // 0.25 s 440 Hz mono 16-bit PCM WAV
            const int rate = 22050, n = rate / 4;
            QByteArray wav;
            auto u32le = [&](quint32 v) { for (int i = 0; i < 4; ++i) wav.append(char((v >> (8 * i)) & 0xff)); };
            auto u16le = [&](quint16 v) { wav.append(char(v & 0xff)); wav.append(char(v >> 8)); };
            wav.append("RIFF");
            u32le(36 + n * 2);
            wav.append("WAVEfmt ");
            u32le(16); u16le(1); u16le(1); u32le(rate); u32le(rate * 2); u16le(2); u16le(16);
            wav.append("data");
            u32le(n * 2);
            for (int i = 0; i < n; ++i) u16le(quint16(qint16(8000 * std::sin(i * 2.0 * 3.14159265 * 440.0 / rate))));
            writeFile(QStringLiteral("Audio/Footstep.wav"), wav);
            writeFile(QStringLiteral("Footstep.wav"), wav);
        }
        QFile::copy(QDir(content).filePath(QStringLiteral("Prefabs/CrateStack.oxprefab")), QDir(content).filePath(QStringLiteral("CrateStack.oxprefab")));
        ctx.services().assets().rescan();
    }
    OX_LOG_INFO("editor", "Loaded project Showcase ({} entities)", ctx.editWorld().entityCount());
    OX_LOG_INFO("assets", "Indexed {} assets in Assets/", 14);
    OX_LOG_WARN("render", "Ray tracing disabled: {}", ctx.services().caps().caps().rayTracingUnavailableReason.toStdString());
    OX_LOG_INFO("scalability", "Overall quality: High");
    OX_LOG_ERROR("script", "onStart: Scripts/Door.lua:12: attempt to index a nil value (field 'door')");
    OX_LOG_DEBUG("physics", "Broadphase rebuilt");

    {
        MainWindow w(&ctx);
        w.resize(1680, 1000);
        w.show();
        settle(200);
        frameCamera(w.viewport());
        ctx.selection().select(findByName(ctx.editWorld(), "Orb"));
        if (auto* d = w.dock(QStringLiteral("dock.content"))) d->raise();
        w.contentBrowser()->navigate(content);
        settle(300);
        save(&w, outputDir, QStringLiteral("main_window_dark.png"), out);

        // multi-selection + rotate gizmo + console
        UuidList pillars;
        for (int i = 1; i <= 3; ++i) pillars.push_back(findByName(ctx.editWorld(), ("Pillar " + std::to_string(i)).c_str()));
        ctx.selection().set(pillars);
        w.viewport()->setGizmoMode(GizmoMode::Rotate);
        w.viewport()->setShowStats(true);
        if (auto* d = w.dock(QStringLiteral("dock.console"))) d->raise();
        settle(250);
        save(&w, outputDir, QStringLiteral("main_window_multiselect_console.png"), out);
        save(w.inspector(), outputDir, QStringLiteral("inspector_multiselect.png"), out);
        save(w.console(), outputDir, QStringLiteral("console.png"), out);
        w.viewport()->setShowStats(false);
        w.viewport()->setGizmoMode(GizmoMode::Translate);

        // prefab instance in the inspector
        ctx.selection().select(findByName(ctx.editWorld(), "Crate B"));
        settle(200);
        save(w.inspector(), outputDir, QStringLiteral("inspector_prefab_overrides.png"), out);
        ctx.selection().select(findByName(ctx.editWorld(), "Sun"));
        settle(200);
        save(w.inspector(), outputDir, QStringLiteral("inspector_light.png"), out);
        if (auto* d = w.dock(QStringLiteral("dock.content"))) d->raise();
        w.contentBrowser()->navigate(QDir(content).filePath(QStringLiteral("Textures")));
        w.contentBrowser()->navigate(content);
        if (auto* d = w.dock(QStringLiteral("dock.content"))) w.resizeDocks({d}, {420}, Qt::Vertical);
        settle(300);
        save(w.contentBrowser(), outputDir, QStringLiteral("content_browser.png"), out);
        if (auto* d = w.dock(QStringLiteral("dock.content"))) w.resizeDocks({d}, {300}, Qt::Vertical);

        // asset inspector (asset database): texture import settings, material values
        ctx.selection().clear();
        ctx.inspectAsset(QDir(content).filePath(QStringLiteral("Textures/Checker_Wood.png")));
        settle(250);
        save(w.inspector(), outputDir, QStringLiteral("inspector_asset_texture.png"), out);
        ctx.inspectAsset(QDir(content).filePath(QStringLiteral("Materials/Brushed Steel.oxmat")));
        settle(250);
        save(w.inspector(), outputDir, QStringLiteral("inspector_asset_material.png"), out);
        ctx.inspectAsset({});

        // gameplay components: script properties, spline point editing
        ctx.selection().select(findByName(ctx.editWorld(), "Orb"));
        settle(250);
        w.inspector()->rebuild();
        for (ComponentCard* c : w.inspector()->cards()) c->setExpanded(qs(c->info()->name) == QLatin1String("Script"));
        settle(150);
        save(w.inspector(), outputDir, QStringLiteral("inspector_script_properties.png"), out);
        for (ComponentCard* c : w.inspector()->cards()) c->setExpanded(true);
        if (Uuid path = findByName(ctx.editWorld(), "Patrol Path"); !path.isNil()) {
            ctx.selection().select(path);
            ctx.tools().setTool(ViewportTool::SplinePoints, path);
            ctx.tools().splinePoint = 2;
            w.viewport()->requestRedraw();
            settle(300);
            save(&w, outputDir, QStringLiteral("main_window_spline_editing.png"), out);
            ctx.tools().reset();
        }

        // play mode: physics, scripts, coroutines, behaviour trees, save games
        ctx.selection().select(findByName(ctx.editWorld(), "Orb"));
        ctx.play().setAutoTick(false);
        ctx.startPlay(PlayMode::Play);
        for (int i = 0; i < 75; ++i) ctx.play().tick(1.0 / 60.0);
        settle(200);
        save(&w, outputDir, QStringLiteral("main_window_playing.png"), out);
#if OX_EDITOR_HAS_ASYNC
        if (auto* sched = ctx.engineServices().tryGet<CoroutineScheduler>()) {
            Entity beacon = ctx.world().findByName("Beacon");
            // A C++ gameplay coroutine with an owner entity next to the Lua script's scene.delay coroutines.
            sched->spawn([]() -> Task<> {
                for (;;) co_await seconds(30.0);
            }, SpawnOptions{.name = "Beacon.Pulse", .owner = beacon ? entityRuntimeId(beacon.handle()) : 0});
            sched->spawn([]() -> Task<> { co_await frames(600); }, SpawnOptions{.name = "Quest.Intro"});
        }
#endif
        for (int i = 0; i < 3; ++i) ctx.play().tick(1.0 / 60.0);
        if (auto* d = w.dock(QStringLiteral("dock.coroutines"))) {
            d->show();
            d->raise();
        }
        w.coroutines()->refresh();
        settle(200);
        save(w.coroutines(), outputDir, QStringLiteral("coroutines_panel.png"), out);
        ctx.selection().select(findByName(ctx.world(), "Guard"));
        for (int i = 0; i < 20; ++i) ctx.play().tick(1.0 / 60.0);
        if (auto* d = w.dock(QStringLiteral("dock.behaviorTree"))) {
            d->show();
            d->raise();
        }
        w.behaviorTree()->refresh();
        settle(200);
        save(w.behaviorTree(), outputDir, QStringLiteral("behavior_tree_debugger.png"), out);
#if OX_EDITOR_HAS_RUNTIME
        if (Engine* e = ctx.engine()) {
            (void)e->saveGame("checkpoint_courtyard", "Courtyard — checkpoint");
            for (int i = 0; i < 30; ++i) ctx.play().tick(1.0 / 60.0);
            (void)e->saveGame("quicksave", "Quick save");
            SaveGameInspector dlg(&ctx, &w);
            dlg.resize(1180, 640);
            dlg.show();
            if (dlg.slots()->topLevelItemCount() > 0) dlg.slots()->setCurrentItem(dlg.slots()->topLevelItem(dlg.slots()->topLevelItemCount() - 1));
            save(&dlg, outputDir, QStringLiteral("save_game_inspector.png"), out);
            dlg.hide();
        }
#endif
        ctx.stopPlay();
        ctx.play().setAutoTick(true);
        if (auto* d = w.dock(QStringLiteral("dock.content"))) d->raise();

        // light theme
        ctx.preferences().modify([](PreferenceValues& v) { v.theme = ThemeMode::Light; });
        ctx.preferences().applyAppearance();
        ctx.selection().select(findByName(ctx.editWorld(), "Orb"));
        settle(300);
        save(&w, outputDir, QStringLiteral("main_window_light.png"), out);
        ctx.preferences().modify([](PreferenceValues& v) { v.theme = ThemeMode::Dark; });
        ctx.preferences().applyAppearance();
        settle(150);

        {
            ProjectSettingsDialog dlg(&ctx, &w);
            dlg.show();
            dlg.showPage(QStringLiteral("rendering"));
            save(&dlg, outputDir, QStringLiteral("project_settings_rendering.png"), out);
            dlg.showPage(QStringLiteral("scalability"));
            scalability::setOverall(QualityLevel::High);
            scalability::setGroup(Scalability::Shadows, QualityLevel::Ultra);
            if (ICVar* c = CVarRegistry::instance().find("r.Textures.MaxAnisotropy")) c->setFromString("16", CVarSource::Console);
            dlg.scalability()->refresh();
            if (auto* b = dlg.findChild<QToolButton*>()) (void)b;
            // expand the Shadows details row
            for (auto* tb : dlg.scalability()->findChildren<QToolButton*>()) {
                if (tb->toolTip().contains(QStringLiteral("console variables"))) {
                    tb->click();
                    break;
                }
            }
            save(&dlg, outputDir, QStringLiteral("project_settings_scalability.png"), out);
            dlg.showPage(QStringLiteral("physics"));
            save(&dlg, outputDir, QStringLiteral("project_settings_physics.png"), out);
            dlg.showPage(QStringLiteral("input"));
            save(&dlg, outputDir, QStringLiteral("project_settings_input.png"), out);
            dlg.showPage(QStringLiteral("general"));
            save(&dlg, outputDir, QStringLiteral("project_settings_general.png"), out);
            dlg.setSearchText(QStringLiteral("shadow"));
            save(&dlg, outputDir, QStringLiteral("project_settings_search.png"), out);
            dlg.hide();
        }
        {
            PreferencesDialog dlg(&ctx, &w);
            dlg.show();
            dlg.showPage(QStringLiteral("appearance"));
            save(&dlg, outputDir, QStringLiteral("preferences_appearance.png"), out);
            dlg.showPage(QStringLiteral("viewport"));
            save(&dlg, outputDir, QStringLiteral("preferences_viewport.png"), out);
            dlg.showPage(QStringLiteral("shortcuts"));
            save(&dlg, outputDir, QStringLiteral("preferences_shortcuts.png"), out);
            if (dlg.page(QStringLiteral("gameUser"))) {
                dlg.showPage(QStringLiteral("gameUser"));
                save(&dlg, outputDir, QStringLiteral("preferences_game_user_settings.png"), out);
            }
            dlg.hide();
        }
        {
            AddComponentPopup popup(&ctx, ctx.selection().ids(), &w);
            popup.resize(320, 380);
            popup.show();
            save(&popup, outputDir, QStringLiteral("add_component_popup.png"), out);
            popup.hide();
        }
        {
            AboutDialog dlg(&ctx, &w);
            dlg.show();
            save(&dlg, outputDir, QStringLiteral("about.png"), out);
            dlg.hide();
        }
        {
            ProjectBrowser dlg(&ctx, &w);
            dlg.show();
            save(&dlg, outputDir, QStringLiteral("project_browser.png"), out);
            dlg.showCreatePage();
            save(&dlg, outputDir, QStringLiteral("project_browser_new.png"), out);
            dlg.hide();
        }
        w.hide();
    }
    {
        SplashScreen splash;
        splash.show();
        splash.setMessage(QObject::tr("Loading project Showcase…"), 72);
        save(&splash, outputDir, QStringLiteral("splash.png"), out);
        splash.hide();
    }
    ctx.preferences().setValues(savedPrefs);
    ctx.preferences().applyAppearance();
    return out;
}

} // namespace ox::editor
