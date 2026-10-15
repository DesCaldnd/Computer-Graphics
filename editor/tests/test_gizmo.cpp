#include "test_common.hpp"

#include "viewport/gizmo.hpp"
#include "viewport/gizmo_math.hpp"
#include "viewport/viewport_panel.hpp"

#include <QMouseEvent>

#include <cmath>

// Oriented (World/Local) transform gizmo: the Qt-free math in viewport/gizmo_math (frames, picking of rotated handles,
// drag projection, snapping in the gizmo frame, applying drags under parents with non-uniform scale), TransformGizmo
// in Local space, and the viewport driving it with mouse events (undo/redo, preference persistence).
namespace ox::editor::test {

namespace {

bool near(glm::vec3 a, glm::vec3 b, float eps = 1e-4f) { return glm::length(a - b) <= eps; }
bool near(float a, float b, float eps = 1e-4f) { return std::abs(a - b) <= eps; }
// same rotation (q and -q are equal)
bool sameRotation(glm::quat a, glm::quat b, float eps = 1e-4f) { return std::abs(std::abs(glm::dot(a, b)) - 1.0f) <= eps; }
glm::vec3 col(const glm::mat4& m, int i) { return glm::vec3(m[i]); }

ViewportCamera testCamera() {
    ViewportCamera cam;
    cam.position = {2.0f, 3.0f, 9.0f};
    cam.yaw = glm::radians(10.0f);
    cam.pitch = glm::radians(-15.0f);
    return cam;
}

glm::vec2 projected(const GizmoView& v, glm::vec3 p) {
    glm::vec2 out(0.0f);
    v.projection().project(p, out);
    return out;
}
QPointF qp(glm::vec2 p) { return {p.x, p.y}; }

// A rotated parent with non-uniform scale (sheared child world matrices) and a rotated, scaled child.
Transform parentTransform() {
    Transform t;
    t.position = {1.0f, 0.5f, -2.0f};
    t.rotation = glm::angleAxis(glm::radians(35.0f), glm::normalize(glm::vec3(0.3f, 1.0f, 0.2f)));
    t.scale = {2.0f, 1.0f, 0.5f};
    return t;
}
Transform childTransform() {
    Transform t;
    t.position = {0.5f, 1.0f, 0.25f};
    t.rotation = glm::angleAxis(glm::radians(50.0f), glm::normalize(glm::vec3(1.0f, 0.2f, -0.4f)));
    t.scale = {1.0f, 1.5f, 0.75f};
    return t;
}

} // namespace

class GizmoTests : public QObject {
    Q_OBJECT
    std::unique_ptr<EditorContext> ctx;

private Q_SLOTS:
    void init() { ctx = makeContext(); }
    void cleanup() { ctx.reset(); }

    // ---- frame ----------------------------------------------------------------------------------------------

    void frameMatchesDecomposeWithoutShear() {
        Transform t;
        t.rotation = glm::angleAxis(1.1f, glm::normalize(glm::vec3(1, 2, 3)));
        t.scale = {3.0f, 0.5f, 2.0f};
        QVERIFY(sameRotation(gizmo::frameRotation(t.toMatrix()), t.rotation));
        // mirrored: folded into X like ox::decompose
        t.scale = {-1.0f, 2.0f, 1.0f};
        QVERIFY(sameRotation(gizmo::frameRotation(t.toMatrix()), Transform::fromMatrix(t.toMatrix()).rotation));
        // zero scale on one axis still gives a valid frame
        t.scale = {1.0f, 0.0f, 1.0f};
        const glm::quat q = gizmo::frameRotation(t.toMatrix());
        QVERIFY(near(glm::length(q), 1.0f));
        QVERIFY(near(q * glm::vec3(1, 0, 0), t.rotation * glm::vec3(1, 0, 0)));
        QVERIFY(sameRotation(gizmo::frameRotation(glm::mat4(0.0f)), glm::quat(1, 0, 0, 0)));
    }

    void frameUnderNonUniformParent() {
        // the child's world matrix is sheared: the frame keeps its visible X axis and XY plane, orthonormal
        const glm::mat4 world = parentTransform().toMatrix() * childTransform().toMatrix();
        const glm::quat q = gizmo::frameRotation(world);
        const glm::vec3 x = gizmo::axisOf(q, 0), y = gizmo::axisOf(q, 1), z = gizmo::axisOf(q, 2);
        QVERIFY(near(x, glm::normalize(col(world, 0))));
        QVERIFY(near(glm::dot(x, y), 0.0f) && near(glm::dot(y, z), 0.0f) && near(glm::dot(x, z), 0.0f));
        QVERIFY(near(glm::dot(z, glm::normalize(glm::cross(col(world, 0), col(world, 1)))), 1.0f));
        QVERIFY(glm::dot(y, col(world, 1)) > 0.0f);
    }

    void scaleIsAlwaysLocal() {
        const glm::quat local = glm::angleAxis(0.7f, glm::vec3(0, 1, 0));
        QVERIFY(sameRotation(gizmo::effectiveOrientation(GizmoMode::Translate, GizmoSpace::World, local), glm::quat(1, 0, 0, 0)));
        QVERIFY(sameRotation(gizmo::effectiveOrientation(GizmoMode::Rotate, GizmoSpace::World, local), glm::quat(1, 0, 0, 0)));
        QVERIFY(sameRotation(gizmo::effectiveOrientation(GizmoMode::Translate, GizmoSpace::Local, local), local));
        QVERIFY(sameRotation(gizmo::effectiveOrientation(GizmoMode::Scale, GizmoSpace::World, local), local));
        TransformGizmo g;
        g.setSpace(GizmoSpace::World);
        g.setTarget({}, local);
        g.setMode(GizmoMode::Translate);
        QVERIFY(near(g.axisDir(0), glm::vec3(1, 0, 0)));
        g.setMode(GizmoMode::Scale);
        QVERIFY(near(g.axisDir(0), local * glm::vec3(1, 0, 0)));
    }

    // ---- picking ----------------------------------------------------------------------------------------------

    void pickRotatedAxesAndPlanes() {
        const GizmoView view = GizmoView::from(testCamera(), {800, 600});
        const gizmo::Projection proj = view.projection();
        gizmo::Frame f;
        f.pivot = {0.0f, 0.0f, 0.0f};
        f.orientation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1)); // local X = world +Y, local Y = world -X
        f.length = 1.5f;
        // along local X (world +Y) → X; along world +X (= local -Y, no handle) → nothing
        QCOMPARE(gizmo::pickTranslate(proj, f, projected(view, {0.0f, 0.9f, 0.0f})), AxisX);
        QCOMPARE(gizmo::pickTranslate(proj, f, projected(view, {0.9f, 0.0f, 0.0f})), AxisNone);
        QCOMPARE(gizmo::pickTranslate(proj, f, projected(view, {-0.9f, 0.0f, 0.0f})), AxisY);
        QCOMPARE(gizmo::pickTranslate(proj, f, projected(view, {0.0f, 0.0f, 0.9f})), AxisZ);
        // plane handle centres
        const GizmoAxis planes[3] = {AxisYZ, AxisXZ, AxisXY};
        for (int n = 0; n < 3; ++n) {
            const auto q = gizmo::planeHandle(f, n);
            QCOMPARE(gizmo::pickTranslate(proj, f, projected(view, (q[0] + q[1] + q[2] + q[3]) * 0.25f)), planes[n]);
        }
        QCOMPARE(gizmo::pickTranslate(proj, f, projected(view, f.pivot)), AxisAll);
        QCOMPARE(gizmo::pickScale(proj, f, projected(view, {0.0f, 1.2f, 0.0f})), AxisX);
        // rotate rings: the ring around local X lies in the plane of local Y/Z (= world X/Z here)
        const float a = 0.8f;
        const glm::vec3 onXRing = (f.axis(1) * std::cos(a) + f.axis(2) * std::sin(a)) * f.length;
        QCOMPARE(gizmo::pickRotate(proj, f, projected(view, onXRing), view.camera.right()), AxisX);
        const glm::vec3 onZRing = (f.axis(0) * std::cos(a) + f.axis(1) * std::sin(a)) * f.length;
        QCOMPARE(gizmo::pickRotate(proj, f, projected(view, onZRing), view.camera.right()), AxisZ);
        // a world-aligned frame picks the other way round
        f.orientation = glm::quat(1, 0, 0, 0);
        QCOMPARE(gizmo::pickTranslate(proj, f, projected(view, {0.9f, 0.0f, 0.0f})), AxisX);
    }

    void gizmoHitTestFollowsLocalSpace() {
        const GizmoView view = GizmoView::from(testCamera(), {800, 600});
        TransformGizmo g;
        g.setMode(GizmoMode::Translate);
        g.setVisible(true);
        const glm::quat rot = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0)); // local X = world -Z
        g.setTarget({0, 0, 0}, rot);
        g.setSpace(GizmoSpace::Local);
        const float len = g.worldLength(view);
        QCOMPARE(g.hitTest(view, qp(projected(view, glm::vec3(0, 0, -1) * len * 0.6f))), AxisX);
        g.setSpace(GizmoSpace::World);
        QCOMPARE(g.hitTest(view, qp(projected(view, glm::vec3(1, 0, 0) * len * 0.6f))), AxisX);
        QVERIFY(g.hitTest(view, qp(projected(view, glm::vec3(0, 0, -1) * len * 0.6f))) != AxisX);
    }

    // ---- drag projection + snapping --------------------------------------------------------------------------

    void dragAlongLocalAxisWithSnap() {
        const GizmoView view = GizmoView::from(testCamera(), {800, 600});
        TransformGizmo g;
        g.setMode(GizmoMode::Translate);
        g.setSpace(GizmoSpace::Local);
        g.setVisible(true);
        const glm::quat rot = glm::angleAxis(glm::radians(40.0f), glm::vec3(0, 1, 0)) * glm::angleAxis(glm::radians(20.0f), glm::vec3(0, 0, 1));
        const glm::vec3 pivot(0.5f, 0.0f, -0.5f);
        g.setTarget(pivot, rot);
        const glm::vec3 x = rot * glm::vec3(1, 0, 0);
        const float len = g.worldLength(view);
        const QPointF start = qp(projected(view, pivot + x * len * 0.6f));
        QCOMPARE(g.hitTest(view, start), AxisX);
        QVERIFY(g.begin(view, start, AxisX));
        auto d = g.update(view, qp(projected(view, pivot + x * (len * 0.6f + 2.0f))));
        QVERIFY(near(d.translation, x * 2.0f, 0.03f)); // world delta along the local axis
        QVERIFY(near(g.pivot(), pivot + d.translation));
        // the frame stays fixed during the drag even if the target is re-set
        g.setTarget({9, 9, 9}, glm::quat(1, 0, 0, 0));
        QVERIFY(near(g.axisDir(0), x));
        // snapping is along the local axis
        g.setSnap({true, 0.5f, 15.0f, 0.1f});
        d = g.update(view, qp(projected(view, pivot + x * (len * 0.6f + 1.2f))));
        QVERIFY(near(d.translation, x * 1.0f, 1e-3f));
        g.end();

        // plane drag: snapped per local axis
        const glm::quat frame = rot;
        const glm::vec3 delta = frame * glm::vec3(0.6f, 0.0f, 1.3f);
        const glm::vec3 snapped = gizmo::planeTranslation(glm::vec3(1.0f), glm::vec3(1.0f) + delta, frame, 0.5f);
        QVERIFY(near(glm::inverse(frame) * snapped, glm::vec3(0.5f, 0.0f, 1.5f)));
        QVERIFY(near(gizmo::planeTranslation({}, delta, frame, 0.0f), delta));
        QVERIFY(near(gizmo::axisTranslation(x, 0.3f, 1.55f, 0.5f), x * 1.5f));
        QVERIFY(near(gizmo::snapTo(-0.74f, 0.5f), -0.5f));
    }

    void rotateAroundLocalAxis() {
        const GizmoView view = GizmoView::from(testCamera(), {800, 600});
        TransformGizmo g;
        g.setMode(GizmoMode::Rotate);
        g.setSpace(GizmoSpace::Local);
        g.setVisible(true);
        const glm::quat rot = glm::angleAxis(glm::radians(30.0f), glm::normalize(glm::vec3(1, 1, 0)));
        g.setTarget({0, 0, 0}, rot);
        const gizmo::Frame f = g.frame(view);
        auto ringPoint = [&](float a) { return f.pivot + (f.axis(0) * std::cos(a) + f.axis(1) * std::sin(a)) * f.length; }; // Z ring
        QCOMPARE(g.hitTest(view, qp(projected(view, ringPoint(0.5f)))), AxisZ);
        QVERIFY(g.begin(view, qp(projected(view, ringPoint(0.5f))), AxisZ));
        const auto d = g.update(view, qp(projected(view, ringPoint(1.0f))));
        const glm::vec3 localZ = rot * glm::vec3(0, 0, 1);
        QVERIFY(near(std::abs(glm::dot(glm::axis(d.rotation), localZ)), 1.0f, 1e-3f));
        QVERIFY(near(d.angleDeg, glm::degrees(0.5f), 0.5f));
        g.setSnap({true, 0.25f, 15.0f, 0.1f});
        QVERIFY(near(g.update(view, qp(projected(view, ringPoint(1.0f)))).angleDeg, 30.0f, 1e-3f));
        g.end();
        QVERIFY(near(gizmo::signedAngle({1, 0, 0}, {0, 1, 0}, {0, 0, 1}), glm::radians(90.0f)));
    }

    // ---- applying drags to transforms ------------------------------------------------------------------------

    void applyUnderNonUniformParent() {
        gizmo::EditTarget t;
        t.local = childTransform();
        t.parentWorld = parentTransform().toMatrix();
        const glm::mat4 world0 = t.worldMatrix();
        const glm::vec3 pos0 = t.worldPosition();

        // translate along the child's local X: world position moves exactly, rotation/scale stay bit-exact
        const glm::vec3 x = gizmo::axisOf(gizmo::frameRotation(world0), 0);
        Transform r = gizmo::applyTranslation(t, x * 1.25f);
        QVERIFY(near(glm::vec3((t.parentWorld * r.toMatrix())[3]), pos0 + x * 1.25f));
        QVERIFY(r.rotation == t.local.rotation && r.scale == t.local.scale);

        // rotate about the child's own (local) Y axis: the pivot stays, the visible X axis turns exactly
        const glm::vec3 y = gizmo::axisOf(gizmo::frameRotation(world0), 1);
        const glm::quat dR = glm::angleAxis(glm::radians(25.0f), y);
        r = gizmo::applyRotation(t, pos0, dR);
        const glm::mat4 world1 = t.parentWorld * r.toMatrix();
        QVERIFY(near(glm::vec3(world1[3]), pos0));
        QVERIFY(near(glm::normalize(col(world1, 0)), dR * glm::normalize(col(world0, 0))));
        QVERIFY(r.position == t.local.position && r.scale == t.local.scale);
        QVERIFY(near(glm::length(r.rotation), 1.0f));
        // exact rotation without shear (uniformly scaled parent): world rotation = dR * old
        gizmo::EditTarget u = t;
        Transform up = parentTransform();
        up.scale = glm::vec3(2.0f);
        u.parentWorld = up.toMatrix();
        const glm::quat w0 = gizmo::frameRotation(u.worldMatrix());
        r = gizmo::applyRotation(u, u.worldPosition(), dR);
        QVERIFY(sameRotation(gizmo::frameRotation(u.parentWorld * r.toMatrix()), dR * w0));
        // a mirrored parent keeps a proper local rotation
        up.scale = {-1.0f, 1.0f, 1.0f};
        u.parentWorld = up.toMatrix();
        const glm::mat4 m0 = u.worldMatrix();
        r = gizmo::applyRotation(u, u.worldPosition(), dR);
        QVERIFY(near(glm::normalize(col(u.parentWorld * r.toMatrix(), 1)), dR * glm::normalize(col(m0, 1)), 1e-3f));

        // rotate about another pivot (multi-selection): the position orbits
        const glm::vec3 pivot = pos0 + glm::vec3(1.0f, 0.0f, 0.0f);
        const glm::quat yaw = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0));
        r = gizmo::applyRotation(t, pivot, yaw);
        QVERIFY(near(glm::vec3((t.parentWorld * r.toMatrix())[3]), pivot + yaw * (pos0 - pivot), 1e-3f));

        // scale: factors along the child's own axes; around the pivot along the gizmo frame
        r = gizmo::applyScale(t, pos0, gizmo::frameRotation(world0), {2.0f, 1.0f, 0.5f}, false);
        QVERIFY(near(r.scale, t.local.scale * glm::vec3(2.0f, 1.0f, 0.5f)));
        QVERIFY(r.position == t.local.position && r.rotation == t.local.rotation);
        const glm::quat frame = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1)); // frame X = world +Y
        r = gizmo::applyScale(t, pos0 - glm::vec3(0.0f, 1.0f, 0.0f), frame, {3.0f, 1.0f, 1.0f}, true);
        QVERIFY(near(glm::vec3((t.parentWorld * r.toMatrix())[3]), pos0 + glm::vec3(0.0f, 2.0f, 0.0f), 1e-3f));
    }

    // ---- viewport (mouse-driven) -----------------------------------------------------------------------------

    void viewportDragsInLocalSpace() {
        World& w = ctx->editWorld();
        const Uuid parentId = ctx->createEntity(QStringLiteral("Parent"));
        const Uuid childId = ctx->createEntity(QStringLiteral("Child"), parentId);
        w.find(parentId).setLocalTransform(parentTransform());
        w.find(childId).setLocalTransform(childTransform());
        w.updateTransforms();
        ctx->selection().select(childId);

        ViewportPanel panel(ctx.get());
        panel.resize(900, 700);
        panel.show();
        pump(50);
        auto* canvas = panel.findChild<ViewportCanvas*>();
        QVERIFY(canvas);
        QVERIFY(canvas->width() > 100 && canvas->height() > 100);
        QCOMPARE(panel.gizmo().space(), GizmoSpace::Local); // default
        panel.setGizmoMode(GizmoMode::Translate);
        ViewportCamera& cam = panel.camera();
        const glm::vec3 pos0 = w.find(childId).worldPosition();
        cam.position = pos0 + glm::vec3(4.0f, 3.0f, 8.0f);
        const glm::vec3 fwd = glm::normalize(pos0 - cam.position);
        cam.yaw = std::atan2(-fwd.x, -fwd.z);
        cam.pitch = std::asin(fwd.y);
        (void)panel.renderToImage(canvas->size()); // paints → gizmo target = the child

        auto send = [&](QEvent::Type type, glm::vec2 p, Qt::MouseButton b, Qt::MouseButtons bs) {
            QMouseEvent e(type, qp(p), canvas->mapToGlobal(qp(p)), b, bs, Qt::NoModifier);
            QCoreApplication::sendEvent(canvas, &e);
        };
        auto drag = [&](glm::vec2 a, glm::vec2 b) {
            send(QEvent::MouseButtonPress, a, Qt::LeftButton, Qt::LeftButton);
            send(QEvent::MouseMove, (a + b) * 0.5f, Qt::NoButton, Qt::LeftButton);
            send(QEvent::MouseMove, b, Qt::NoButton, Qt::LeftButton);
            send(QEvent::MouseButtonRelease, b, Qt::LeftButton, Qt::NoButton);
        };

        // the gizmo follows the child's world frame (parent hierarchy included)
        const GizmoView view = GizmoView::from(cam, glm::vec2(float(canvas->width()), float(canvas->height())));
        const glm::mat4 world0 = w.find(childId).worldMatrix();
        const glm::quat frame0 = gizmo::frameRotation(world0);
        QVERIFY(sameRotation(panel.gizmo().orientation(), frame0));
        QVERIFY(near(panel.gizmo().pivot(), pos0));

        // translate along local X
        const glm::vec3 x = gizmo::axisOf(frame0, 0);
        const float len = panel.gizmo().worldLength(view);
        const int undoBefore = ctx->undoStack().count();
        drag(projected(view, pos0 + x * len * 0.6f), projected(view, pos0 + x * (len * 0.6f + 1.5f)));
        w.updateTransforms();
        const Transform afterMove = w.find(childId).localTransform().toTransform();
        QVERIFY(near(w.find(childId).worldPosition(), pos0 + x * 1.5f, 0.03f));
        QVERIFY(afterMove.rotation == childTransform().rotation && afterMove.scale == childTransform().scale);
        QCOMPARE(ctx->undoStack().count(), undoBefore + 1); // one command per drag

        // rotate about the local Y axis (ring spanned by local Z and X)
        panel.setGizmoMode(GizmoMode::Rotate);
        (void)panel.renderToImage(canvas->size());
        const glm::vec3 pos1 = w.find(childId).worldPosition();
        const gizmo::Frame f = panel.gizmo().frame(view);
        QVERIFY(sameRotation(f.orientation, frame0));
        auto ring = [&](float a) { return projected(view, f.pivot + (f.axis(2) * std::cos(a) + f.axis(0) * std::sin(a)) * f.length); };
        QCOMPARE(panel.gizmo().hitTest(view, qp(ring(0.6f))), AxisY);
        drag(ring(0.6f), ring(1.2f));
        w.updateTransforms();
        const glm::mat4 world2 = w.find(childId).worldMatrix();
        const glm::vec3 y = gizmo::axisOf(frame0, 1);
        QVERIFY(near(glm::vec3(world2[3]), pos1, 1e-3f));
        const glm::quat expected = glm::angleAxis(0.6f, y);
        QVERIFY(near(glm::normalize(col(world2, 0)), expected * x, 0.02f)); // visible X turned about the local Y
        QVERIFY(near(gizmo::axisOf(gizmo::frameRotation(world2), 1), y, 0.02f)); // the local Y itself stays
        QVERIFY(w.find(childId).localTransform().scale == childTransform().scale);
        // the gizmo follows the new rotation on the next frame
        (void)panel.renderToImage(canvas->size());
        QVERIFY(sameRotation(panel.gizmo().orientation(), gizmo::frameRotation(world2), 1e-3f));

        // inspector-style edit (property command) → gizmo follows
        ctx->setProperty({childId}, "Transform", "rotation", serial::Value::makeQuat(glm::angleAxis(glm::radians(45.0f), glm::vec3(0, 1, 0))));
        w.updateTransforms();
        (void)panel.renderToImage(canvas->size());
        QVERIFY(sameRotation(panel.gizmo().orientation(), gizmo::frameRotation(w.find(childId).worldMatrix()), 1e-3f));
        ctx->undoStack().undo();

        // undo restores rotation, then translation, exactly
        ctx->undoStack().undo();
        QVERIFY(w.find(childId).localTransform().toTransform() == afterMove);
        ctx->undoStack().undo();
        QVERIFY(w.find(childId).localTransform().toTransform() == childTransform());
        ctx->undoStack().redo();
        QVERIFY(w.find(childId).localTransform().toTransform() == afterMove);

        // World space: translate gizmo world-aligned, scale gizmo still local
        panel.setGizmoSpace(GizmoSpace::World);
        panel.setGizmoMode(GizmoMode::Translate);
        (void)panel.renderToImage(canvas->size());
        QVERIFY(sameRotation(panel.gizmo().orientation(), glm::quat(1, 0, 0, 0)));
        panel.setGizmoMode(GizmoMode::Scale);
        (void)panel.renderToImage(canvas->size());
        QVERIFY(sameRotation(panel.gizmo().orientation(), frame0));
    }

    void spacePersistsInPreferences() {
        QVERIFY(PreferenceValues{}.gizmoLocalSpace); // default Local
        ViewportPanel panel(ctx.get());
        QCOMPARE(panel.gizmo().space(), GizmoSpace::Local);
        QSignalSpy spy(&panel, &ViewportPanel::gizmoSpaceChanged);
        panel.toggleGizmoSpace();
        QCOMPARE(panel.gizmo().space(), GizmoSpace::World);
        QCOMPARE(spy.count(), 1);
        QVERIFY(!ctx->preferences().values().gizmoLocalSpace);
        const QJsonObject json = ctx->preferences().values().toJson();
        QCOMPARE(json["viewport"].toObject()["gizmoSpace"].toString(), QStringLiteral("World"));
        PreferenceValues back;
        back.fromJson(json);
        QVERIFY(!back.gizmoLocalSpace);
        // a preference change (Preferences dialog) switches the viewport
        ctx->preferences().modify([](PreferenceValues& v) { v.gizmoLocalSpace = true; });
        QCOMPARE(panel.gizmo().space(), GizmoSpace::Local);
        QCOMPARE(spy.count(), 2);
    }
};

OX_EDITOR_TEST(GizmoTests);

} // namespace ox::editor::test

#include "test_gizmo.moc"
