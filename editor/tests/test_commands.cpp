#include "test_common.hpp"

#include "panels/outliner_panel.hpp"
#include "viewport/gizmo.hpp"

#include <oxwald/scene/prefab.hpp>

namespace ox::editor::test {

class CommandTests : public QObject {
    Q_OBJECT
    std::unique_ptr<EditorContext> ctx;
    World& w() { return ctx->editWorld(); }

private Q_SLOTS:
    void init() { ctx = makeContext(); }
    void cleanup() { ctx.reset(); }

    void propertyEditUndoRedo() {
        Entity e = w().create("Box");
        const Uuid id = e.uuid();
        ctx->setProperty({id}, "Transform", "position", serial::Value::makeVec3({1, 2, 3}));
        QCOMPARE(w().find(id).localTransform().position, glm::vec3(1, 2, 3));
        QVERIFY(ctx->isDirty());
        ctx->undoStack().undo();
        QCOMPARE(w().find(id).localTransform().position, glm::vec3(0));
        ctx->undoStack().redo();
        QCOMPARE(w().find(id).localTransform().position, glm::vec3(1, 2, 3));
        // unchanged value does not create a command
        const int n = ctx->undoStack().count();
        ctx->setProperty({id}, "Transform", "position", serial::Value::makeVec3({1, 2, 3}));
        QCOMPARE(ctx->undoStack().count(), n);
    }

    void dragEditsMerge() {
        const Uuid id = w().create("Box").uuid();
        const int before = ctx->undoStack().count();
        ctx->setProperty({id}, "Transform", "position.x", serial::Value::makeF64(0.5), EditPhase::Begin);
        for (int i = 1; i <= 10; ++i) ctx->setProperty({id}, "Transform", "position.x", serial::Value::makeF64(0.5 + i), EditPhase::Update);
        ctx->setProperty({id}, "Transform", "position.x", serial::Value::makeF64(20.0), EditPhase::End);
        QCOMPARE(ctx->undoStack().count(), before + 1);
        QCOMPARE(w().find(id).localTransform().position.x, 20.0f);
        // a following edit is a new command (the drag was closed)
        ctx->setProperty({id}, "Transform", "position.x", serial::Value::makeF64(21.0), EditPhase::Begin);
        QCOMPARE(ctx->undoStack().count(), before + 2);
        ctx->undoStack().undo();
        ctx->undoStack().undo();
        QCOMPARE(w().find(id).localTransform().position.x, 0.0f);
    }

    void createDeleteKeepsIdsAndOrder() {
        const Uuid parent = ctx->createEntity(QStringLiteral("Parent"));
        const Uuid c1 = ctx->createEntity(QStringLiteral("C1"), parent);
        const Uuid c2 = ctx->createEntity(QStringLiteral("C2"), parent);
        const Uuid c3 = ctx->createEntity(QStringLiteral("C3"), parent);
        w().find(c2).add<LightComponent>().intensity = 1234.0f;
        ctx->deleteEntities({c2});
        QVERIFY(!w().find(c2));
        ctx->undoStack().undo();
        Entity restored = w().find(c2);
        QVERIFY(restored);
        QCOMPARE(restored.parent().uuid(), parent);
        QCOMPARE(restored.get<LightComponent>().intensity, 1234.0f);
        auto kids = w().find(parent).children();
        QCOMPARE(kids.size(), size_t(3));
        QCOMPARE(kids[0].uuid(), c1);
        QCOMPARE(kids[1].uuid(), c2);
        QCOMPARE(kids[2].uuid(), c3);
        // deleting the parent deletes and restores the subtree
        ctx->deleteEntities({parent, c1});
        QCOMPARE(w().entityCount(), usize(0));
        ctx->undoStack().undo();
        QCOMPARE(w().entityCount(), usize(4));
        // undo creation
        while (ctx->undoStack().canUndo()) ctx->undoStack().undo();
        QCOMPARE(w().entityCount(), usize(0));
        for (int i = 0; i < 4; ++i) ctx->undoStack().redo(); // the four creations
        QCOMPARE(w().entityCount(), usize(4));
        QVERIFY(w().find(c3)); // same UUID after redo
        QCOMPARE(w().find(c3).parent().uuid(), parent);
        ctx->undoStack().redo(); // the subtree deletion
        QCOMPARE(w().entityCount(), usize(0));
    }

    void reparentUndo() {
        Entity a = w().create("A");
        Entity b = w().create("B");
        b.setPosition({5, 0, 0});
        a.setPosition({1, 1, 1});
        const Uuid ida = a.uuid(), idb = b.uuid();
        ctx->reparentEntities({idb}, ida);
        QCOMPARE(w().find(idb).parent().uuid(), ida);
        // world position preserved
        QVERIFY(glm::length(w().find(idb).worldPosition() - glm::vec3(5, 0, 0)) < 1e-4f);
        // cycles are rejected
        const int n = ctx->undoStack().count();
        ctx->reparentEntities({ida}, idb);
        QCOMPARE(ctx->undoStack().count(), n);
        ctx->undoStack().undo();
        QVERIFY(!w().find(idb).parent());
        QCOMPARE(w().find(idb).localTransform().position, glm::vec3(5, 0, 0));
        QCOMPARE(w().rootHandles()[1], w().find(idb).handle());
    }

    void duplicateCopyPaste() {
        const Uuid id = ctx->createEntity(CreateKind::Cube);
        const UuidList dup = ctx->duplicateEntities({id});
        QCOMPARE(dup.size(), size_t(1));
        QVERIFY(dup[0] != id);
        QVERIFY(w().find(dup[0]).has<MeshRendererComponent>());
        ctx->copy({id});
        const UuidList pasted = ctx->paste();
        QCOMPARE(pasted.size(), size_t(1));
        QCOMPARE(w().entityCount(), usize(3));
        ctx->undoStack().undo();
        ctx->undoStack().undo();
        QCOMPARE(w().entityCount(), usize(1));
    }

    void outlinerReflectsWorldChanges() {
        OutlinerPanel outliner(ctx.get());
        const Uuid id = ctx->createEntity(QStringLiteral("Hero"));
        pump();
        QModelIndex i = outliner.model()->indexOf(id);
        QVERIFY(i.isValid());
        QCOMPARE(i.data().toString(), QStringLiteral("Hero"));
        ctx->renameEntity(id, QStringLiteral("Villain"));
        pump();
        QCOMPARE(outliner.model()->indexOf(id).data().toString(), QStringLiteral("Villain"));
        // selection syncs to the view
        QVERIFY(outliner.view()->selectionModel()->selectedRows().size() == 1);
        const Uuid child = ctx->createEntity(QStringLiteral("Sword"), id);
        pump();
        QCOMPARE(outliner.model()->indexOf(child).parent(), outliner.model()->indexOf(id));
        ctx->deleteEntities({id});
        pump();
        QVERIFY(!outliner.model()->indexOf(id).isValid());
        QVERIFY(!outliner.model()->indexOf(child).isValid());
        ctx->undoStack().undo();
        pump();
        QVERIFY(outliner.model()->indexOf(child).isValid());
        // drag & drop: drop "Sword" on the root reparents it
        QMimeData* mime = outliner.model()->mimeData({outliner.model()->indexOf(child)});
        outliner.model()->dropMimeData(mime, Qt::MoveAction, -1, 0, {});
        delete mime;
        pump();
        QVERIFY(!w().find(child).parent());
    }

    void playCloneDoesNotMutateEditWorld() {
        Entity e = w().create("Mover");
        e.setPosition({1, 0, 0});
        const Uuid id = e.uuid();
        const int undoCount = ctx->undoStack().count();
        ctx->play().setAutoTick(false);
        QVERIFY(ctx->startPlay(PlayMode::Play));
        QVERIFY(ctx->isPlaying());
        QVERIFY(&ctx->world() != &ctx->editWorld());
        QVERIFY(ctx->world().find(id)); // same UUIDs in the clone
        ctx->setProperty({id}, "Transform", "position", serial::Value::makeVec3({9, 9, 9}));
        ctx->deleteEntities({id});
        ctx->createEntity(QStringLiteral("Spawned"));
        ctx->play().tick(1.0 / 60.0);
        QCOMPARE(ctx->undoStack().count(), undoCount); // play edits are not recorded
        ctx->stopPlay();
        QVERIFY(!ctx->isPlaying());
        QVERIFY(w().find(id));
        QCOMPARE(w().find(id).localTransform().position, glm::vec3(1, 0, 0));
        QCOMPARE(w().entityCount(), usize(1));
        // pause/step
        QVERIFY(ctx->startPlay(PlayMode::Simulate));
        ctx->play().setPaused(true);
        const u64 frames = ctx->play().frames();
        ctx->play().step();
        QCOMPARE(ctx->play().frames(), frames + 1);
        ctx->stopPlay();
    }

    void sceneSaveLoadAndDirty() {
        auto project = Project::createTemporary(QStringLiteral("SaveTest"));
        QVERIFY(project);
        const QString dir = project->contentDir();
        ctx->setProject(std::move(project));
        ctx->newScene(true);
        QVERIFY(!ctx->isDirty());
        ctx->createEntity(QStringLiteral("Thing"));
        QVERIFY(ctx->isDirty());
        const QString bin = QDir(dir).filePath(QStringLiteral("Scenes/Test.oxscene"));
        QVERIFY(ctx->saveScene(bin));
        QVERIFY(!ctx->isDirty());
        const QString json = QDir(dir).filePath(QStringLiteral("Scenes/Test.oxscene.json"));
        QVERIFY(ctx->saveScene(json));
        QFile f(json);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(f.readAll().contains("oxb1-json"));
        const usize count = w().entityCount();
        QVERIFY(ctx->openScene(bin));
        QCOMPARE(w().entityCount(), count);
        QVERIFY(w().findByName("Thing"));
    }

    void prefabOverridesAndRevert() {
        auto project = Project::createTemporary(QStringLiteral("PrefabTest"));
        const QString dir = project->contentDir();
        ctx->setProject(std::move(project));
        ctx->newScene(false);
        const Uuid root = ctx->createEntity(QStringLiteral("Lamp"));
        ctx->addComponent({root}, "Light");
        const QString path = QDir(dir).filePath(QStringLiteral("Prefabs/Lamp.oxprefab"));
        QVERIFY(ctx->createPrefab(root, path));
        QVERIFY(w().find(root).has<PrefabInstanceComponent>());
        const Uuid inst = ctx->instantiatePrefab(path);
        QVERIFY(!inst.isNil());
        ctx->setProperty({inst}, "Light", "intensity", serial::Value::makeF64(5.0));
        QVERIFY(ctx->isOverridden(inst, "Light.intensity"));
        ctx->revertPrefabOverride(inst, "Light.intensity");
        QVERIFY(!ctx->isOverridden(inst, "Light.intensity"));
        QCOMPARE(w().find(inst).get<LightComponent>().intensity, LightComponent{}.intensity);
        ctx->undoStack().undo(); // revert is undoable
        QCOMPARE(w().find(inst).get<LightComponent>().intensity, 5.0f);
    }

    void gizmoTranslateAlongAxis() {
        TransformGizmo g;
        g.setMode(GizmoMode::Translate);
        g.setTarget({0, 0, 0}, glm::quat(1, 0, 0, 0));
        g.setVisible(true);
        ViewportCamera cam;
        cam.position = {0, 2, 10};
        cam.yaw = 0;
        cam.pitch = glm::radians(-10.0f);
        const GizmoView view = GizmoView::from(cam, {800, 600});
        QPointF origin, tip;
        QVERIFY(view.project({0, 0, 0}, origin));
        QVERIFY(view.project({g.worldLength(view) * 0.6f, 0, 0}, tip));
        QCOMPARE(g.hitTest(view, tip), AxisX);
        QVERIFY(g.begin(view, tip, AxisX));
        QPointF target;
        view.project({g.worldLength(view) * 0.6f + 2.0f, 0, 0}, target);
        const auto d = g.update(view, target);
        QVERIFY(std::abs(d.translation.x - 2.0f) < 0.05f);
        QVERIFY(std::abs(d.translation.y) < 1e-4f);
        g.setSnap({true, 0.5f, 15.0f, 0.1f});
        view.project({g.worldLength(view) * 0.6f + 1.2f, 0, 0}, target);
        QVERIFY(std::abs(g.update(view, target).translation.x - 1.0f) < 1e-3f);
        g.end();
        QVERIFY(!g.active());
    }
};

OX_EDITOR_TEST(CommandTests);

} // namespace ox::editor::test

#include "test_commands.moc"
