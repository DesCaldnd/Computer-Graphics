// Engine module integration: runtime projects + Engine, play-in-editor through gameplay, the asset database
// content browser, script properties, coroutines panel, project settings -> .oxproj.
#include "test_common.hpp"

#include "content/thumbnails.hpp"
#include "core/log_capture.hpp"
#include "dialogs/save_game_inspector.hpp"
#include "inspector/asset_inspector.hpp"
#include "inspector/component_card.hpp"
#include "inspector/component_extensions.hpp"
#include "inspector/inspector_panel.hpp"
#include "inspector/reflected_object_editor.hpp"
#include "integration/gameplay_tools.hpp"
#include "panels/behavior_tree_panel.hpp"
#include "panels/console_panel.hpp"
#include "panels/content_browser.hpp"
#include "panels/coroutines_panel.hpp"
#include "settings/project_settings_dialog.hpp"
#include "settings/scalability_widget.hpp"
#include "theme/icons.hpp"
#include "widgets/widgets.hpp"

#include <oxwald/core/scalability.hpp>
#include <oxwald/scene/component_registry.hpp>
#include <oxwald/scene/components.hpp>
#include <oxwald/scene/runtime_id.hpp>

#if OX_EDITOR_HAS_RUNTIME
#include <oxwald/runtime/engine.hpp>
#include <oxwald/runtime/project.hpp>
#endif
#if OX_EDITOR_HAS_GAMEPLAY
#include <oxwald/gameplay/gameplay.hpp>
#endif
#if OX_EDITOR_HAS_ASYNC
#include <oxwald/async/scheduler.hpp>
#endif
#if OX_EDITOR_HAS_ASSETS
#include <oxwald/assets/asset_meta.hpp>
#endif

#include <QListView>
#include <QMimeData>
#include <QSignalSpy>
#include <QTreeWidget>
#include <QUrl>

namespace ox::editor::test {

namespace {

QString freshDir(const QString& name) {
    const QString dir = QDir(tempRoot()).filePath(name);
    QDir(dir).removeRecursively();
    QDir().mkpath(dir);
    return dir;
}

QString writePng(const QString& path, QColor color = QColor(200, 80, 40)) {
    QImage img(64, 32, QImage::Format_RGBA8888);
    img.fill(color);
    for (int x = 0; x < 64; x += 8) img.setPixelColor(x, 5, Qt::white);
    QDir().mkpath(QFileInfo(path).absolutePath());
    img.save(path);
    return path;
}

void writeText(const QString& path, const QByteArray& text) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) f.write(text);
}

template <class T>
T* findChildNamed(QObject* root, const QString& name) {
    return root->findChild<T*>(name);
}

} // namespace

class IntegrationTests : public QObject {
    Q_OBJECT
    std::unique_ptr<EditorContext> ctx;

    World& w() { return ctx->editWorld(); }

    void useProject(const QString& name) {
        auto p = Project::create(freshDir(name), name, QStringLiteral("Blank"), nullptr);
        QVERIFY(p);
        ctx->setProject(std::move(p));
    }

private Q_SLOTS:
    void init() { ctx = makeContext(); }
    void cleanup() { ctx.reset(); }

    void projectCreateOpenRoundTrip() {
        const QString parent = freshDir(QStringLiteral("roundtrip"));
        QString err;
        auto p = Project::create(parent, QStringLiteral("RoundTrip"), QStringLiteral("Blank"), &err);
        QVERIFY2(p, qPrintable(err));
        const QString file = p->projectFile();
        QVERIFY(file.endsWith(QLatin1String(".oxproj")));
        QVERIFY(QFileInfo::exists(file));
        QCOMPARE(QFileInfo(p->contentDir()).fileName(), QStringLiteral("Assets"));
        QVERIFY(QFileInfo::exists(QDir(p->contentDir()).filePath(QStringLiteral("Scenes/Main.oxscene"))));
        p->setSetting("version", "1.2.3");
        p->setSetting("editor.network.port", 9100);
        QVERIFY(p->save());
#if OX_EDITOR_HAS_RUNTIME
        // The runtime reads exactly what the editor wrote.
        auto rt = ox::Project::load(file.toStdString());
        QVERIFY(rt);
        QCOMPARE(rt->settings.name, std::string("RoundTrip"));
        QCOMPARE(rt->settings.version, std::string("1.2.3"));
        QCOMPARE(rt->settings.assetDirs.front(), std::string("Assets"));
        QCOMPARE(rt->settings.startupScene, std::string("project://Assets/Scenes/Main.oxscene"));
        QVERIFY(!rt->settings.input.actions.empty());
#endif
        auto reopened = Project::open(file, &err);
        QVERIFY2(reopened, qPrintable(err));
        QCOMPARE(reopened->name(), QStringLiteral("RoundTrip"));
        QCOMPARE(reopened->setting("version").get<std::string>(), std::string("1.2.3"));
        QCOMPARE(reopened->setting("editor.network.port").get<int>(), 9100);
        QCOMPARE(reopened->pathForUri(QStringLiteral("project://Assets/Scenes/Main.oxscene")),
                 QDir(reopened->contentDir()).filePath(QStringLiteral("Scenes/Main.oxscene")));
        // Opening the project starts the editor's Engine on it.
        ctx->setProject(std::move(reopened));
#if OX_EDITOR_HAS_RUNTIME
        QVERIFY(ctx->engine());
        QCOMPARE(ctx->engine()->projectSettings().name, std::string("RoundTrip"));
        QVERIFY(ctx->engine()->config().editor);
        QCOMPARE(ctx->engine()->mode(), EngineMode::Edit);
#endif
#if OX_EDITOR_HAS_ASSETS && OX_EDITOR_HAS_RUNTIME
        QVERIFY(ctx->services().assets().isDatabase());
        QCOMPARE(QDir::cleanPath(ctx->services().assets().rootPath()), QDir::cleanPath(ctx->project()->contentDir()));
#endif
        // Directory form + legacy editor projects are converted.
        QVERIFY(Project::open(QFileInfo(file).absolutePath(), &err));
        const QString legacyDir = freshDir(QStringLiteral("legacy"));
        writeText(QDir(legacyDir).filePath(QStringLiteral("Old.oxproject")), R"({"name":"Old","startupScene":"Scenes/Main.oxscene"})");
        QDir().mkpath(QDir(legacyDir).filePath(QStringLiteral("Content")));
        auto legacy = Project::open(QDir(legacyDir).filePath(QStringLiteral("Old.oxproject")), &err);
        QVERIFY2(legacy, qPrintable(err));
        QVERIFY(legacy->projectFile().endsWith(QLatin1String("Old.oxproj")));
        QCOMPARE(legacy->assetDirName(), QStringLiteral("Content"));
    }

    void playStopKeepsEditWorldWhileGameplayRuns() {
#if OX_EDITOR_HAS_GAMEPLAY && OX_EDITOR_HAS_RUNTIME
        useProject(QStringLiteral("PlayPhysics"));
        QVERIFY(ctx->engine());
        auto ids = ctx->createEntities(QStringLiteral("Physics"), [](World& world) {
            Entity ground = world.create("Ground");
            auto& gc = ground.add<gameplay::ColliderComponent>();
            gc.type = gameplay::ColliderType::Box;
            gc.halfExtents = {10.0f, 0.5f, 10.0f};
            ground.setPosition({0, -0.5f, 0});
            Entity ball = world.create("Ball");
            ball.setPosition({0, 5, 0});
            auto& bc = ball.add<gameplay::ColliderComponent>();
            bc.type = gameplay::ColliderType::Sphere;
            bc.radius = 0.5f;
            ball.add<gameplay::RigidBodyComponent>().motionType = physics::MotionType::Dynamic;
            return std::vector<Entity>{ground, ball};
        });
        QCOMPARE(int(ids.size()), 2);
        const Uuid ball = ids[1];
        const int undo = ctx->undoStack().count();
        ctx->play().setAutoTick(false);
        QVERIFY(ctx->startPlay(PlayMode::Play));
        QCOMPARE(ctx->engine()->mode(), EngineMode::Play);
        QVERIFY(&ctx->world() != &ctx->editWorld());
        for (int i = 0; i < 45; ++i) ctx->play().tick(1.0 / 60.0);
        const float yPlay = ctx->world().find(ball).worldPosition().y;
        QVERIFY2(yPlay < 4.0f, qPrintable(QString::number(yPlay))); // gravity acted on the rigid body
        QVERIFY(ctx->play().frames() >= 45);
        ctx->stopPlay();
        QCOMPARE(ctx->engine()->mode(), EngineMode::Edit);
        QCOMPARE(ctx->editWorld().find(ball).localTransform().position, glm::vec3(0, 5, 0)); // edit world untouched
        QCOMPARE(ctx->undoStack().count(), undo);
        // Simulate: physics runs, gameplay logic systems are switched off and restored afterwards.
        QVERIFY(ctx->startPlay(PlayMode::Simulate));
        QVERIFY(!ctx->engine()->scheduler().isEnabled(gameplay::systems::kScriptUpdate));
        for (int i = 0; i < 30; ++i) ctx->play().tick(1.0 / 60.0);
        QVERIFY(ctx->world().find(ball).worldPosition().y < 4.8f);
        ctx->play().setPaused(true);
        const float paused = ctx->world().find(ball).worldPosition().y;
        ctx->play().tick(1.0 / 60.0);
        QCOMPARE(ctx->world().find(ball).worldPosition().y, paused);
        ctx->play().step();
        QVERIFY(ctx->world().find(ball).worldPosition().y < paused);
        ctx->stopPlay();
        QVERIFY(ctx->engine()->scheduler().isEnabled(gameplay::systems::kScriptUpdate));
        QCOMPARE(ctx->editWorld().find(ball).localTransform().position, glm::vec3(0, 5, 0));
#else
        QSKIP("gameplay/runtime modules not linked");
#endif
    }

    void contentBrowserImportsPngWithMetaAndThumbnail() {
#if OX_EDITOR_HAS_ASSETS && OX_EDITOR_HAS_RUNTIME
        useProject(QStringLiteral("ImportTest"));
        IAssetBackend& backend = ctx->services().assets();
        QVERIFY(backend.isDatabase());
        const QString outside = writePng(QDir(freshDir(QStringLiteral("finder"))).filePath(QStringLiteral("brick.png")));
        ContentBrowserPanel browser(ctx.get());
        const QString textures = QDir(ctx->project()->contentDir()).filePath(QStringLiteral("Textures"));
        browser.navigate(textures);
        // Finder drop onto the browser (text/uri-list)
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(outside)});
        QVERIFY(browser.model()->canDropMimeData(&mime, Qt::CopyAction, -1, -1, {}));
        browser.model()->dropMimeData(&mime, Qt::CopyAction, -1, -1, {});
        const QString imported = QDir(textures).filePath(QStringLiteral("brick.png"));
        QVERIFY(QFileInfo::exists(imported));
        QVERIFY(QFileInfo::exists(imported + QStringLiteral(".meta")));
        auto info = backend.info(imported);
        QVERIFY(info);
        QCOMPARE(info->type, QStringLiteral("Texture"));
        QCOMPARE(info->importer, QStringLiteral("texture"));
        QVERIFY(info->imported);
        auto meta = assets::readMeta((imported + QStringLiteral(".meta")).toStdString());
        QVERIFY(meta);
        QCOMPARE(meta->uuid, info->uuid);
        // thumbnail (memory + disk cache)
        const QModelIndex idx = browser.model()->indexOfPath(imported);
        QVERIFY(idx.isValid());
        const QPixmap thumb = idx.data(AssetListModel::ThumbRole).value<QPixmap>();
        QVERIFY(!thumb.isNull());
        QVERIFY(QFileInfo::exists(ThumbnailCache::cacheFile(*ctx, *info)));
        // import settings through reflection + reimport
        AssetInspector inspector(ctx.get());
        inspector.setAsset(imported);
        QVERIFY(inspector.settingsEditor());
        QVERIFY(inspector.settingsEditor()->setField("maxSize", serial::Value::makeInt(16)));
        QVERIFY(inspector.applyImportSettings());
        QCOMPARE(assets::readMeta((imported + QStringLiteral(".meta")).toStdString())->settings["maxSize"].get<int>(), 16);
        // dependency-aware delete: a material referencing the texture
        const QString mat = QDir(ctx->project()->contentDir()).filePath(QStringLiteral("Materials/Brick.oxmat"));
        writeText(mat, QStringLiteral(R"({"oxmat":1,"albedoTexture":"%1"})").arg(qs(info->uuid.toString())).toUtf8());
        backend.rescan();
        QVERIFY(backend.reimport(mat, nullptr));
        const QList<AssetInfo> users = backend.dependents(imported);
        QCOMPARE(users.size(), 1);
        QCOMPARE(QFileInfo(users.front().path).fileName(), QStringLiteral("Brick.oxmat"));
        // rename keeps the UUID (.meta travels with the file)
        QString err;
        const QString renamed = backend.rename(imported, QStringLiteral("wall"), &err);
        QVERIFY2(!renamed.isEmpty(), qPrintable(err));
        QCOMPARE(backend.info(renamed)->uuid, info->uuid);
        QVERIFY(QFileInfo::exists(renamed + QStringLiteral(".meta")));
        // created assets get metas (Lua script template, behaviour tree)
        const QString script = backend.createAsset(QDir(ctx->project()->contentDir()).filePath(QStringLiteral("Scripts")), QStringLiteral("Script"), QStringLiteral("Hero"), &err);
        QVERIFY(QFileInfo::exists(script + QStringLiteral(".meta")));
        QCOMPARE(backend.info(script)->type, QStringLiteral("Script"));
        const QString bt = backend.createAsset(QDir(ctx->project()->contentDir()).filePath(QStringLiteral("AI")), QStringLiteral("BehaviorTree"), QStringLiteral("Guard"), &err);
        QVERIFY(backend.reimport(bt, &err));
        QCOMPARE(backend.info(bt)->type, QStringLiteral("BehaviorTree"));
#else
        QSKIP("assets/runtime modules not linked");
#endif
    }

    void scriptPropertiesAppearInInspector() {
#if OX_EDITOR_HAS_GAMEPLAY && OX_EDITOR_HAS_RUNTIME
        useProject(QStringLiteral("ScriptProps"));
        writeText(QDir(ctx->project()->contentDir()).filePath(QStringLiteral("Scripts/Mover.lua")), R"(
properties = {
    speed = { type = "float", default = 3, min = 0, max = 10, tooltip = "metres per second", order = 1 },
    loop = { type = "bool", default = true, order = 2 },
    label = { type = "string", default = "mover", order = 3 },
}
function onUpdate(self, dt) end
)");
        ctx->services().assets().rescan();
        auto ids = ctx->createEntities(QStringLiteral("Scripted"), [](World& world) {
            Entity e = world.create("Mover");
            e.add<gameplay::ScriptComponent>().script = "Scripts/Mover.lua";
            return std::vector<Entity>{e};
        });
        const Uuid id = ids.front();
        const ScriptPropertiesResult props = scriptProperties(*ctx, id);
        QVERIFY2(props.error.isEmpty(), qPrintable(props.error));
        QCOMPARE(int(props.rows.size()), 3);
        QCOMPARE(props.rows[0].name, std::string("speed"));
        QCOMPARE(*props.rows[0].max, 10.0);

        InspectorPanel inspector(ctx.get());
        ctx->selection().select(id);
        inspector.rebuild();
        ComponentCard* card = inspector.card(QStringLiteral("Script"));
        QVERIFY(card);
        QVERIFY(!card->editorForPath("properties")); // the raw map is replaced by typed fields
        QVERIFY(card->footer());
        auto* speed = card->footer()->findChild<PropertyEditor*>(QStringLiteral("scriptprop:speed"));
        QVERIFY(speed);
        auto* field = speed->findChild<NumberField*>();
        QVERIFY(field);
        QCOMPARE(field->value(), 3.0);
        QVERIFY(card->footer()->findChild<PropertyEditor*>(QStringLiteral("scriptprop:loop")));
        QVERIFY(card->footer()->findChild<PropertyEditor*>(QStringLiteral("scriptprop:label")));
        Q_EMIT field->edited(7.5, EditPhase::Single);
        const auto& sc = w().find(id).get<gameplay::ScriptComponent>();
        QVERIFY(sc.properties.contains("speed"));
        QCOMPARE(sc.properties.at("speed").number, 7.5);
        QVERIFY(scriptProperties(*ctx, id).rows[0].overridden);
        ctx->undoStack().undo();
        QVERIFY(!w().find(id).get<gameplay::ScriptComponent>().properties.contains("speed"));
#else
        QSKIP("gameplay/runtime modules not linked");
#endif
    }

    void coroutinePanelListsRunningCoroutine() {
#if OX_EDITOR_HAS_ASYNC && OX_EDITOR_HAS_RUNTIME
        auto* sched = ctx->engineServices().tryGet<CoroutineScheduler>();
        QVERIFY(sched);
        const Uuid owner = ctx->createEntity(QStringLiteral("Door"));
        Entity e = ctx->world().find(owner);
        CoroutineHandle h = sched->spawn([]() -> Task<> { co_await seconds(100.0); },
                                         SpawnOptions{.name = "Door.Open", .owner = entityRuntimeId(e.handle())});
        QVERIFY(h.isRunning());
        CoroutinesPanel panel(ctx.get());
        panel.refresh();
        QVERIFY(panel.count() >= 1);
        QTreeWidgetItem* item = nullptr;
        for (int i = 0; i < panel.tree()->topLevelItemCount(); ++i) {
            if (panel.tree()->topLevelItem(i)->text(0) == QLatin1String("Door.Open")) item = panel.tree()->topLevelItem(i);
        }
        QVERIFY(item);
        QCOMPARE(item->text(1), QStringLiteral("Door"));
        QVERIFY(item->text(3).contains(QStringLiteral("seconds")));
        item->setSelected(true);
        findChildNamed<QToolButton>(&panel, QStringLiteral("coroutines.cancel"))->click();
        QVERIFY(h.isDone());
        panel.refresh();
        for (int i = 0; i < panel.tree()->topLevelItemCount(); ++i) QVERIFY(panel.tree()->topLevelItem(i)->text(0) != QLatin1String("Door.Open"));
#else
        QSKIP("async/runtime modules not linked");
#endif
    }

    void settingsDialogWritesProjectSettings() {
        useProject(QStringLiteral("SettingsOxproj"));
        {
            ProjectSettingsDialog dlg(ctx.get());
            auto* rate = dlg.findChild<NumberField*>(QStringLiteral("setting:project:physics.fixedRate"));
            QVERIFY(rate);
            Q_EMIT rate->edited(90, EditPhase::Single);
            auto* ai = dlg.findChild<ToggleSwitch*>(QStringLiteral("setting:project:modules.ai"));
            QVERIFY(ai);
            QVERIFY(ai->isChecked()); // missing in the file = enabled
            ai->setChecked(false);
            auto* port = dlg.findChild<NumberField*>(QStringLiteral("setting:project:editor.network.port"));
            QVERIFY(port);
            Q_EMIT port->edited(9001, EditPhase::Single);
            dlg.scalability()->overallControl()->button(0)->click(); // Low
            dlg.applyButton()->click();
        }
        QFile f(ctx->project()->projectFile());
        QVERIFY(f.open(QIODevice::ReadOnly));
        const auto j = nlohmann::json::parse(f.readAll().toStdString());
        QCOMPARE(j["editor"]["network"]["port"].get<int>(), 9001); // editor-only data kept in the same file
#if OX_EDITOR_HAS_RUNTIME
        auto rt = ox::Project::load(ctx->project()->projectFile().toStdString());
        QVERIFY(rt);
        QCOMPARE(rt->settings.physics.fixedRate, 90.0f);
        QVERIFY(!rt->settings.moduleEnabled("ai"));
        QVERIFY(rt->settings.moduleEnabled("physics"));
        QCOMPARE(rt->settings.defaultQuality, std::string("Low"));
        QVERIFY(!rt->settings.input.contexts.empty() && !rt->settings.input.contexts.front().bindings.empty());
#endif
        scalability::setOverall(QualityLevel::High);
    }

    void consoleSourceLinksAndEngineCommands() {
        useProject(QStringLiteral("ConsoleLinks"));
        const QString lua = QDir(ctx->project()->contentDir()).filePath(QStringLiteral("Scripts/Broken.lua"));
        writeText(lua, "error('x')\n");
        ConsolePanel console(ctx.get());
        QCOMPARE(console.resolveSource(QStringLiteral("Scripts/Broken.lua")), QFileInfo(lua).absoluteFilePath());
        QCOMPARE(console.resolveSource(QStringLiteral("Broken.lua")), QFileInfo(lua).absoluteFilePath());
        console.resize(900, 300);
        console.show();
        LogCapture::instance().append(log::Level::Error, QStringLiteral("script"), QStringLiteral("onUpdate: Scripts/Broken.lua:1: x"));
        pump(50);
        QSignalSpy spy(ctx.get(), &EditorContext::openSourceRequested);
        const QModelIndex last = console.view()->model()->index(console.view()->model()->rowCount() - 1, 0);
        const QRect r = console.view()->visualRect(last);
        for (int x = r.left(); x < r.right() && spy.isEmpty(); x += 4) console.openLinkAt(QPoint(x, r.top() + 8));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.front().at(0).toString(), QFileInfo(lua).absoluteFilePath());
        QCOMPARE(spy.front().at(1).toInt(), 1);
#if OX_EDITOR_HAS_RUNTIME
        // engine console commands (runtime Console) + cvars
        QVERIFY(console.execute(QStringLiteral("timescale 0.5")).contains(QStringLiteral("0.5")));
        QCOMPARE(ctx->engine()->timeScale(), 0.5);
        ctx->engine()->setTimeScale(1.0);
#endif
        console.hide();
    }

    void renderComponentsAndUpscalersInEditor() {
#if OX_EDITOR_HAS_RENDER
        for (const char* name : {"PostProcessVolume", "FogVolume", "VolumetricFog", "CloudLayer"}) {
            const ComponentInfo* info = ComponentRegistry::instance().find(name);
            QVERIFY2(info, name);
            QCOMPARE(qs(info->category), QStringLiteral("Rendering"));
            QVERIFY2(Icons::exists(Icons::forComponent(qs(info->icon), qs(info->name))), name);
            QVERIFY(Icons::forComponent(qs(info->icon), qs(info->name)) != QLatin1String("component"));
        }
        const RenderingCaps caps = ctx->services().caps().caps();
        QVERIFY(caps.upscalers.size() >= 4);
        QVERIFY(caps.upscalers[1].available);                 // FSR 1 everywhere
        QVERIFY(caps.upscalers[3].available);                 // TAAU everywhere
        QCOMPARE(caps.upscalers[2].available, caps.dlssSupported);
        if (!caps.upscalers[2].available) QVERIFY(!caps.upscalers[2].reason.isEmpty());
#else
        QSKIP("render module not linked");
#endif
    }

    void gameplayToolsSplineBtAndSaveGames() {
#if OX_EDITOR_HAS_GAMEPLAY && OX_EDITOR_HAS_RUNTIME
        useProject(QStringLiteral("GameplayTools"));
        auto ids = ctx->createEntities(QStringLiteral("Tools"), [](World& world) {
            Entity path = world.create("Path");
            auto& sp = path.add<gameplay::SplineComponent>();
            for (int i = 0; i < 3; ++i) {
                gameplay::SplinePoint p;
                p.position = {float(i), 0, 0};
                sp.points.push_back(p);
            }
            Entity guard = world.create("Guard");
            guard.add<gameplay::BehaviorTreeComponent>().treeJson =
                R"({"root":{"type":"Sequence","name":"Duty","children":[{"type":"Wait","name":"Idle","seconds":10}]}})";
            Entity crate = world.create("Crate");
            auto& mr = crate.add<MeshRendererComponent>();
            mr.mesh = builtin::cubeMesh();
            crate.setScale(glm::vec3(2.0f));
            crate.add<gameplay::ColliderComponent>().halfExtents = glm::vec3(0.1f);
            return std::vector<Entity>{path, guard, crate};
        });
        // spline point editing through undoable commands
        QCOMPARE(int(splinePointsWorld(w(), ids[0]).size()), 3);
        const int at = insertSplinePoint(*ctx, ids[0], 0, {0.5f, 1, 0});
        QCOMPARE(at, 1);
        QCOMPARE(int(splinePointsWorld(w(), ids[0]).size()), 4);
        setSplinePointWorld(*ctx, ids[0], 3, {5, 0, 0}, EditPhase::Single);
        QCOMPARE(splinePointsWorld(w(), ids[0])[3], glm::vec3(5, 0, 0));
        removeSplinePoint(*ctx, ids[0], 0);
        QCOMPARE(int(splinePointsWorld(w(), ids[0]).size()), 3);
        ctx->undoStack().undo();
        ctx->undoStack().undo();
        QCOMPARE(splinePointsWorld(w(), ids[0])[3], glm::vec3(2, 0, 0));
        // collider fitted to the (unit) cube mesh in local space
        QVERIFY(fitColliderToMesh(*ctx, {ids[2]}));
        QCOMPARE(w().find(ids[2]).get<gameplay::ColliderComponent>().halfExtents, glm::vec3(0.5f));
        // behaviour tree: static structure in edit mode, live statuses while playing
        BtSnapshot edit = behaviorTreeSnapshot(ctx->engineServices(), w(), ids[1]);
        QVERIFY(edit.hasComponent && !edit.running);
        QCOMPARE(int(edit.nodes.size()), 2);
        ctx->play().setAutoTick(false);
        QVERIFY(ctx->startPlay(PlayMode::Play));
        for (int i = 0; i < 5; ++i) ctx->play().tick(1.0 / 60.0);
        BtSnapshot live = behaviorTreeSnapshot(ctx->engineServices(), ctx->world(), ids[1]);
        QVERIFY(live.running);
        QCOMPARE(int(live.nodes.size()), 2);
        QCOMPARE(live.nodes[1].status, 1); // Wait is Running
        BehaviorTreePanel panel(ctx.get());
        ctx->selection().select(ids[1]);
        panel.refresh();
        QCOMPARE(panel.nodes()->topLevelItemCount(), 1);
        QCOMPARE(panel.nodes()->topLevelItem(0)->child(0)->text(2), QStringLiteral("Running"));
        // save game of the play world shows up in the inspector as JSON
        auto saved = ctx->engine()->saveGame("editor_test", "Editor Test");
        QVERIFY(saved);
        ctx->stopPlay();
        SaveGameInspector saves(ctx.get());
        QTreeWidgetItem* slot = nullptr;
        for (int i = 0; i < saves.slots()->topLevelItemCount(); ++i) {
            if (saves.slots()->topLevelItem(i)->text(0) == QLatin1String("editor_test")) slot = saves.slots()->topLevelItem(i);
        }
        QVERIFY(slot);
        saves.slots()->setCurrentItem(slot);
        QVERIFY(saves.json().contains(QStringLiteral("\"savegame\"")) || saves.json().contains(QStringLiteral("savegame")));
        QVERIFY(saves.json().contains(QStringLiteral("Editor Test")));
#else
        QSKIP("gameplay/runtime modules not linked");
#endif
    }
};

OX_EDITOR_TEST(IntegrationTests);

} // namespace ox::editor::test

#include "test_integration.moc"
