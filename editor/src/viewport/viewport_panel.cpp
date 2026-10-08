#include "viewport/viewport_panel.hpp"

#include "core/editor_context.hpp"
#include "inspector/property_editors.hpp"
#include "theme/icons.hpp"
#include "theme/theme.hpp"
#include "widgets/widgets.hpp"

#include <oxwald/core/serial/convert.hpp>
#include <oxwald/scene/components.hpp>

#include <QActionGroup>
#include <QApplication>
#include <QDragEnterEvent>
#include <QGuiApplication>
#include <oxwald/core/log.hpp>
#include "viewport/vulkan_viewport.hpp"
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidgetAction>

#include <cmath>

namespace ox::editor {

// ---- canvas -------------------------------------------------------------------------------------------------

ViewportCanvas::ViewportCanvas(ViewportPanel* panel) : QWidget(panel), m_panel(panel) {
    setObjectName(QStringLiteral("ViewportCanvas"));
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAcceptDrops(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(160, 120);
}

void ViewportCanvas::paintEvent(QPaintEvent*) {
    QPainter p(this);
    m_panel->paintCanvas(p, size());
}
void ViewportCanvas::mousePressEvent(QMouseEvent* e) { m_panel->onMousePress(e); }
void ViewportCanvas::mouseMoveEvent(QMouseEvent* e) { m_panel->onMouseMove(e); }
void ViewportCanvas::mouseReleaseEvent(QMouseEvent* e) { m_panel->onMouseRelease(e); }
void ViewportCanvas::wheelEvent(QWheelEvent* e) { m_panel->onWheel(e); }
void ViewportCanvas::keyPressEvent(QKeyEvent* e) {
    m_panel->onKey(e, true);
    if (!e->isAccepted()) QWidget::keyPressEvent(e);
}
void ViewportCanvas::keyReleaseEvent(QKeyEvent* e) { m_panel->onKey(e, false); }
void ViewportCanvas::focusOutEvent(QFocusEvent* e) {
    m_panel->m_keys.clear();
    m_panel->m_flying = false;
    QWidget::focusOutEvent(e);
}
void ViewportCanvas::resizeEvent(QResizeEvent* e) {
    m_panel->requestRedraw();
    QWidget::resizeEvent(e);
}
bool ViewportCanvas::event(QEvent* e) {
    // While flying, WASD/QE drive the camera instead of triggering tool shortcuts.
    if (e->type() == QEvent::ShortcutOverride && m_panel->m_flying) {
        e->accept();
        return true;
    }
    return QWidget::event(e);
}

void ViewportCanvas::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData()->hasFormat(kMimeAsset)) e->acceptProposedAction();
}
void ViewportCanvas::dropEvent(QDropEvent* e) {
    for (const QString& line : QString::fromUtf8(e->mimeData()->data(kMimeAsset)).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        m_panel->dropAsset(line, e->position());
    }
    e->acceptProposedAction();
}

// ---- panel --------------------------------------------------------------------------------------------------

ViewportPanel::ViewportPanel(EditorContext* ctx, QWidget* parent) : QWidget(parent), m_ctx(ctx) {
    setObjectName(QStringLiteral("ViewportPanel"));
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    if (const auto& factory = ctx->services().viewportRendererFactory()) m_renderer = factory();
    if (!m_renderer || !m_renderer->usesPainter()) {
        // GPU renderers draw through the Vulkan surface path (see rhi integration); the canvas keeps the
        // software renderer as fallback/overlay.
        auto painter = std::make_unique<PainterViewportRenderer>();
        m_painterRenderer = painter.get();
        if (!m_renderer) m_renderer = std::move(painter);
        else m_painterOwned = std::move(painter);
    } else {
        m_painterRenderer = dynamic_cast<PainterViewportRenderer*>(m_renderer.get());
    }
    buildToolbar();
    lay->addWidget(m_toolbar);
#if OX_EDITOR_HAS_RHI
    const QString backend = ctx->preferences().values().viewportBackend;
    const QString platform = QGuiApplication::platformName();
    const bool headless = platform == QLatin1String("offscreen") || platform == QLatin1String("minimal");
    if (!headless && backend != QLatin1String("Software") && !qEnvironmentVariableIsSet("OX_EDITOR_NO_VULKAN")) {
        m_vkWindow = new VulkanViewportWindow(this);
        m_surface = QWidget::createWindowContainer(m_vkWindow, this);
        m_surface->setFocusPolicy(Qt::StrongFocus);
        m_surface->setMinimumSize(160, 120);
        connect(m_vkWindow, &VulkanViewportWindow::failed, this, &ViewportPanel::switchToSoftware);
        VulkanViewportHub::setPending(true);
    }
#endif
    if (!m_surface) {
        m_canvas = new ViewportCanvas(this);
        m_surface = m_canvas;
    }
    lay->addWidget(m_surface, 1);

    const auto& prefs = ctx->preferences().values();
    m_camera.verticalFovDeg = float(prefs.cameraFov);
    m_gizmo.setMode(GizmoMode::Translate);
    TransformGizmo::Snap snap;
    snap.translate = float(prefs.translateSnap);
    snap.rotateDeg = float(prefs.rotateSnap);
    snap.scale = float(prefs.scaleSnap);
    m_gizmo.setSnap(snap);
    m_gizmo.setSizeScale(float(prefs.gizmoSize));

    connect(&m_timer, &QTimer::timeout, this, &ViewportPanel::tick);
    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.start(8);
    m_clock.start();
    m_fpsClock.start();
    auto redraw = [this] { m_dirty = true; };
    connect(ctx, &EditorContext::propertiesChanged, this, redraw);
    connect(ctx, &EditorContext::structureChanged, this, redraw);
    connect(ctx, &EditorContext::worldReset, this, redraw);
    connect(ctx, &EditorContext::visibilityChanged, this, redraw);
    connect(ctx, &EditorContext::playStateChanged, this, redraw);
    connect(&ctx->selection(), &Selection::changed, this, redraw);
    connect(&Theme::instance(), &Theme::changed, this, redraw);
    connect(&ctx->preferences(), &EditorPreferences::changed, this, [this] {
        const auto& v = m_ctx->preferences().values();
        TransformGizmo::Snap s = m_gizmo.snap();
        s.translate = float(v.translateSnap);
        s.rotateDeg = float(v.rotateSnap);
        s.scale = float(v.scaleSnap);
        m_gizmo.setSnap(s);
        m_gizmo.setSizeScale(float(v.gizmoSize));
        m_camera.verticalFovDeg = float(v.cameraFov);
        if (m_speedLabel) m_speedLabel->setText(QStringLiteral("%1 m/s").arg(v.cameraSpeed, 0, 'f', v.cameraSpeed < 10 ? 1 : 0));
        m_dirty = true;
    });
}

ViewportPanel::~ViewportPanel() = default;

void ViewportPanel::setSurfaceCursor(Qt::CursorShape c) {
#if OX_EDITOR_HAS_RHI
    if (m_vkWindow) {
        m_vkWindow->setCursor(c);
        return;
    }
#endif
    m_canvas->setCursor(c);
}

void ViewportPanel::unsetSurfaceCursor() {
#if OX_EDITOR_HAS_RHI
    if (m_vkWindow) {
        m_vkWindow->unsetCursor();
        return;
    }
#endif
    m_canvas->unsetCursor();
}

void ViewportPanel::switchToSoftware(const QString& reason) {
    OX_LOG_WARN("editor", "Vulkan viewport unavailable ({}), using the software preview", reason.toStdString());
    VulkanViewportHub::setPending(false);
    if (!m_vkWindow) return;
    auto* lay = static_cast<QVBoxLayout*>(layout());
    QWidget* old = m_surface;
    m_vkWindow = nullptr;
    m_canvas = new ViewportCanvas(this);
    m_surface = m_canvas;
    lay->replaceWidget(old, m_canvas);
    old->deleteLater();
    m_dirty = true;
    Q_EMIT m_ctx->statusMessage(tr("Vulkan viewport unavailable: %1").arg(reason), 6000);
}

void ViewportPanel::renderGpuFrame(const ViewportTarget& target, QSize sizePx) {
    World& world = m_ctx->world();
    world.updateTransforms();
    DebugDraw& dd = m_ctx->debugDraw();
    updateGizmoTarget();
    // gizmo, grid and selection go to the renderer as overlay lines
    const GizmoView gv = GizmoView::from(m_camera, glm::vec2(sizePx.width(), sizePx.height()));
    if (!m_ctx->isPlaying()) m_gizmo.emitLines(dd, gv);
    if (m_flags.grid) dd.grid(glm::vec3(0), float(m_ctx->preferences().values().gridSize), 100, glm::vec4(1, 1, 1, 0.15f));
    dd.flush(0.016f);
    ViewportFrame frame = makeFrame(sizePx);
    const UuidList hidden = m_ctx->hiddenIds();
    frame.hidden = hidden;
    frame.lines = &dd;
    frame.time = m_clock.elapsed() / 1000.0;
    m_renderer->render(frame, target);
}

QString ViewportPanel::rendererName() const { return m_renderer ? m_renderer->name() : QString(); }

void ViewportPanel::buildToolbar() {
    m_toolbar = new QFrame(this);
    m_toolbar->setObjectName(QStringLiteral("ViewportToolbar"));
    static_cast<QFrame*>(m_toolbar)->setProperty("role", "panelHeader");
    auto* l = new QHBoxLayout(m_toolbar);
    l->setContentsMargins(6, 4, 6, 4);
    l->setSpacing(4);

    auto textButton = [this](const QString& icon, const QString& text, const QString& tip) {
        auto* b = new QToolButton(m_toolbar);
        b->setIcon(Icons::get(icon));
        b->setText(text);
        b->setToolTip(tip);
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        b->setPopupMode(QToolButton::InstantPopup);
        b->setProperty("role", "text");
        b->setIconSize(QSize(15, 15));
        return b;
    };

    // view mode
    m_viewModeButton = textButton(QStringLiteral("sun"), viewModeName(m_viewMode), tr("View mode"));
    auto* vm = new QMenu(m_viewModeButton);
    auto* group = new QActionGroup(vm);
    for (int i = 0; i < int(ViewMode::Count); ++i) {
        const auto mode = ViewMode(i);
        if (mode == ViewMode::BufferBaseColor) vm->addSection(tr("Buffer Visualization"));
        QAction* a = vm->addAction(viewModeName(mode));
        a->setCheckable(true);
        a->setChecked(mode == m_viewMode);
        a->setShortcut(i < 6 ? QKeySequence(Qt::ALT | Qt::Key(Qt::Key_1 + i)) : QKeySequence());
        a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        group->addAction(a);
        addAction(a);
        connect(a, &QAction::triggered, this, [this, mode] { setViewMode(mode); });
    }
    m_viewModeButton->setMenu(vm);
    l->addWidget(m_viewModeButton);

    // camera
    m_cameraButton = textButton(QStringLiteral("perspective"), tr("Perspective"), tr("Camera projection and settings"));
    auto* cm = new QMenu(m_cameraButton);
    connect(cm, &QMenu::aboutToShow, this, [this, cm] {
        cm->clear();
        auto* persp = cm->addAction(tr("Perspective"), this, [this] {
            m_camera.orthographic = false;
            m_cameraButton->setText(tr("Perspective"));
            m_dirty = true;
        });
        persp->setCheckable(true);
        persp->setChecked(!m_camera.orthographic);
        auto* ortho = cm->addAction(tr("Orthographic"), this, [this] {
            m_camera.orthographic = true;
            m_camera.orthoHeight = std::max(2.0f, glm::length(m_camera.position - pivotPoint()));
            m_cameraButton->setText(tr("Orthographic"));
            m_dirty = true;
        });
        ortho->setCheckable(true);
        ortho->setChecked(m_camera.orthographic);
        cm->addSection(tr("Views"));
        struct V {
            QString name;
            float yaw, pitch;
        };
        for (const V& v : {V{tr("Top"), 0.0f, -89.9f}, V{tr("Front"), 0.0f, 0.0f}, V{tr("Right"), 90.0f, 0.0f}, V{tr("Back"), 180.0f, 0.0f}}) {
            cm->addAction(v.name, this, [this, v] {
                const glm::vec3 pivot = pivotPoint();
                const float dist = std::max(4.0f, glm::length(m_camera.position - pivot));
                m_camera.yaw = glm::radians(v.yaw);
                m_camera.pitch = glm::radians(v.pitch);
                m_camera.position = pivot - m_camera.forward() * dist;
                m_dirty = true;
            });
        }
        cm->addSection(tr("Field of View"));
        auto* fovAction = new QWidgetAction(cm);
        auto* fov = new NumberField(cm);
        fov->setRange(10, 150);
        fov->setStep(0.5);
        fov->setDecimals(1);
        fov->setSuffix(QStringLiteral("°"));
        fov->setValue(m_camera.verticalFovDeg);
        fov->setMinimumWidth(160);
        connect(fov, &NumberField::edited, this, [this](double v, EditPhase) {
            m_camera.verticalFovDeg = float(v);
            m_dirty = true;
        });
        fovAction->setDefaultWidget(fov);
        cm->addAction(fovAction);
        cm->addSection(tr("Camera Speed"));
        auto* speedAction = new QWidgetAction(cm);
        auto* speed = new NumberField(cm);
        speed->setRange(0.1, 200);
        speed->setStep(0.1);
        speed->setDecimals(1);
        speed->setSuffix(QStringLiteral(" m/s"));
        speed->setValue(m_ctx->preferences().values().cameraSpeed);
        connect(speed, &NumberField::edited, this, [this](double v, EditPhase) {
            m_ctx->preferences().modify([v](PreferenceValues& p) { p.cameraSpeed = v; });
            Q_EMIT cameraSpeedChanged(v);
        });
        speedAction->setDefaultWidget(speed);
        cm->addAction(speedAction);
    });
    m_cameraButton->setMenu(cm);
    l->addWidget(m_cameraButton);

    // show flags
    auto* showButton = textButton(QStringLiteral("eye"), tr("Show"), tr("Show flags"));
    auto* sm = new QMenu(showButton);
    connect(sm, &QMenu::aboutToShow, this, [this, sm] {
        sm->clear();
        struct F {
            QString name;
            bool ShowFlags::*flag;
        };
        for (const F& f : {F{tr("Grid"), &ShowFlags::grid}, F{tr("Gizmos"), &ShowFlags::gizmos}, F{tr("Icons"), &ShowFlags::icons},
                           F{tr("Debug Draw"), &ShowFlags::debugDraw}, F{tr("Bounds"), &ShowFlags::bounds}, F{tr("Lights"), &ShowFlags::lights},
                           F{tr("Cameras"), &ShowFlags::cameras}, F{tr("Fog"), &ShowFlags::fog}, F{tr("Shadows"), &ShowFlags::shadows},
                           F{tr("Post Processing"), &ShowFlags::postProcess}}) {
            QAction* a = sm->addAction(f.name);
            a->setCheckable(true);
            a->setChecked(m_flags.*(f.flag));
            auto member = f.flag;
            connect(a, &QAction::toggled, this, [this, member](bool on) {
                m_flags.*member = on;
                m_dirty = true;
            });
        }
    });
    showButton->setMenu(sm);
    l->addWidget(showButton);
    l->addStretch(1);

    auto* speedIcon = new QLabel(m_toolbar);
    speedIcon->setPixmap(Icons::get(QStringLiteral("speed"), Icons::Tint::Faint).pixmap(QSize(14, 14)));
    speedIcon->setToolTip(tr("Camera speed (hold right mouse button and scroll to change)"));
    m_speedLabel = new QLabel(m_toolbar);
    m_speedLabel->setProperty("role", "dim");
    const double sp = m_ctx->preferences().values().cameraSpeed;
    m_speedLabel->setText(QStringLiteral("%1 m/s").arg(sp, 0, 'f', sp < 10 ? 1 : 0));
    m_speedLabel->setToolTip(speedIcon->toolTip());
    l->addWidget(speedIcon);
    l->addWidget(m_speedLabel);
    l->addSpacing(8);
    auto* grid = makeToolButton(QStringLiteral("grid"), tr("Toggle grid"), m_toolbar, true);
    grid->setChecked(true);
    connect(grid, &QToolButton::toggled, this, [this](bool on) {
        m_flags.grid = on;
        m_dirty = true;
    });
    l->addWidget(grid);
    auto* stats = makeToolButton(QStringLiteral("stats"), tr("Show frame statistics in the viewport"), m_toolbar, true);
    connect(stats, &QToolButton::toggled, this, &ViewportPanel::setShowStats);
    l->addWidget(stats);
    auto* maximize = makeToolButton(QStringLiteral("focus"), tr("Focus selection (F)"), m_toolbar);
    connect(maximize, &QToolButton::clicked, this, &ViewportPanel::focusSelection);
    l->addWidget(maximize);
}

void ViewportPanel::setGizmoMode(GizmoMode mode) {
    if (m_gizmo.mode() == mode) return;
    m_gizmo.setMode(mode);
    m_dirty = true;
    Q_EMIT gizmoModeChanged(mode);
}

void ViewportPanel::setGizmoSpace(GizmoSpace space) {
    if (m_gizmo.space() == space) return;
    m_gizmo.setSpace(space);
    m_dirty = true;
    Q_EMIT gizmoSpaceChanged(space);
}

void ViewportPanel::setSnapEnabled(bool enabled) {
    auto s = m_gizmo.snap();
    if (s.enabled == enabled) return;
    s.enabled = enabled;
    m_gizmo.setSnap(s);
    Q_EMIT snapChanged(enabled);
}

void ViewportPanel::setViewMode(ViewMode mode) {
    m_viewMode = mode;
    if (m_viewModeButton) {
        m_viewModeButton->setText(viewModeName(mode));
        m_viewModeButton->setIcon(Icons::get(mode == ViewMode::Wireframe ? QStringLiteral("wireframe")
                                             : isBufferView(mode)        ? QStringLiteral("layers")
                                                                         : QStringLiteral("sun")));
    }
    m_dirty = true;
}

void ViewportPanel::setShowStats(bool on) {
    m_flags.stats = on;
    m_dirty = true;
}

glm::vec3 ViewportPanel::pivotPoint() const {
    World& w = m_ctx->world();
    AABB box;
    for (const auto& id : m_ctx->selection().ids()) {
        if (Entity e = w.find(id)) box.expand(entityBounds(w, e));
    }
    if (box.valid()) return box.center();
    // point in front of the camera on the ground, or 10 m ahead
    const glm::vec3 f = m_camera.forward();
    if (f.y < -0.05f) {
        const float t = -m_camera.position.y / f.y;
        if (t > 0 && t < 200) return m_camera.position + f * t;
    }
    return m_camera.position + f * 10.0f;
}

void ViewportPanel::focusSelection() {
    World& w = m_ctx->world();
    AABB box;
    for (const auto& id : m_ctx->selection().ids()) {
        if (Entity e = w.find(id)) box.expand(entityBounds(w, e));
    }
    if (!box.valid()) return;
    const float radius = std::max(0.5f, glm::length(box.extents()));
    const float dist = radius / std::sin(toRadians(m_camera.verticalFovDeg) * 0.5f) * 1.1f;
    m_camera.position = box.center() - m_camera.forward() * dist;
    if (m_camera.orthographic) m_camera.orthoHeight = radius * 3.0f;
    m_dirty = true;
}

GizmoView ViewportPanel::gizmoView() const {
    return GizmoView::from(m_camera, glm::vec2(float(m_surface->width()), float(m_surface->height())));
}

void ViewportPanel::updateGizmoTarget() {
    World& w = m_ctx->world();
    Entity primary = w.find(m_ctx->selection().primary());
    const bool visible = primary && !m_ctx->isLocked(primary.uuid()) && m_flags.gizmos;
    m_gizmo.setVisible(visible);
    if (!visible || m_gizmo.active()) return;
    m_gizmo.setTarget(primary.worldPosition(), primary.worldRotation());
}

ViewportFrame ViewportPanel::makeFrame(QSize size) {
    ViewportFrame f;
    f.world = &m_ctx->world();
    f.camera = m_camera;
    f.sizePx = glm::uvec2(std::max(1, size.width()), std::max(1, size.height()));
    f.view = m_camera.view();
    f.projection = m_camera.projection(float(f.sizePx.x) / float(f.sizePx.y));
    f.viewProjection = f.projection * f.view;
    f.devicePixelRatio = float(devicePixelRatioF());
    f.viewMode = m_viewMode;
    f.showFlags = m_flags;
    f.selection = m_ctx->selection().ids();
    f.playMode = m_ctx->isPlaying();
    const QColor sc = m_ctx->preferences().values().selectionColor;
    f.selectionColor = {sc.redF(), sc.greenF(), sc.blueF(), 1.0f};
    return f;
}

void ViewportPanel::paintCanvas(QPainter& p, QSize size, bool forScreenshot) {
    QElapsedTimer t;
    t.start();
    World& world = m_ctx->world();
    world.updateTransforms();
    DebugDraw& dd = m_ctx->debugDraw();
    if (m_flags.bounds) {
        world.forEachInHierarchy([&](entt::entity h) {
            Entity e = world.wrap(h);
            if (e.has<MeshRendererComponent>()) dd.aabb(entityBounds(world, e), glm::vec4(0.3f, 0.9f, 0.9f, 0.6f));
        });
    }
    dd.flush(0.016f);
    ViewportFrame frame = makeFrame(size);
    const UuidList hidden = m_ctx->hiddenIds();
    frame.hidden = hidden;
    frame.lines = &dd;
    frame.time = m_clock.elapsed() / 1000.0;
    if (m_painterRenderer) {
        const auto& prefs = m_ctx->preferences().values();
        m_painterRenderer->setGridColor(prefs.gridColor);
        m_painterRenderer->setGridSize(float(prefs.gridSize));
        ViewportTarget target;
        target.painter = &p;
        m_painterRenderer->render(frame, target);
    }
    updateGizmoTarget();
    if (!m_ctx->isPlaying() || forScreenshot) m_gizmo.draw(p, GizmoView::from(m_camera, glm::vec2(size.width(), size.height())));
    paintOverlay(p, size);
    m_frameMs = double(t.nsecsElapsed()) / 1e6;
    ++m_fpsFrames;
    if (m_fpsClock.elapsed() >= 500) {
        m_fps = m_fpsFrames * 1000.0 / double(m_fpsClock.restart());
        m_fpsFrames = 0;
    }
    Q_EMIT frameRendered(m_frameMs);
}

void ViewportPanel::paintOverlay(QPainter& p, QSize size) {
    const ThemePalette& c = colors();
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    const int w = size.width(), h = size.height();
    // orientation triad (bottom-left)
    {
        const QPointF o(46, h - 46);
        const glm::mat3 view = glm::mat3(m_camera.view());
        struct Ax {
            int i;
            glm::vec3 v;
        };
        std::vector<Ax> axes = {{0, view * glm::vec3(1, 0, 0)}, {1, view * glm::vec3(0, 1, 0)}, {2, view * glm::vec3(0, 0, 1)}};
        std::sort(axes.begin(), axes.end(), [](const Ax& a, const Ax& b) { return a.v.z < b.v.z; });
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(c.bg0, 120));
        p.drawEllipse(o, 38, 38);
        static const char* names[] = {"X", "Y", "Z"};
        for (const Ax& a : axes) {
            const QPointF e = o + QPointF(a.v.x, -a.v.y) * 26.0;
            QColor col = Theme::axisColor(a.i);
            if (a.v.z < -0.2f) col = col.darker(140);
            p.setPen(QPen(col, 2.2, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(o, e);
            p.setPen(Qt::NoPen);
            p.setBrush(col);
            p.drawEllipse(e, 7, 7);
            p.setPen(QColor(15, 15, 20));
            QFont f = p.font();
            f.setPixelSize(9);
            f.setBold(true);
            p.setFont(f);
            p.drawText(QRectF(e.x() - 7, e.y() - 7, 14, 14), Qt::AlignCenter, QString::fromLatin1(names[a.i]));
        }
    }
    // play border
    if (m_ctx->isPlaying()) {
        const bool sim = m_ctx->play().mode() == PlayMode::Simulate;
        const bool paused = m_ctx->play().state() == PlaySession::State::Paused;
        const QColor bc = paused ? c.warning : sim ? c.info : c.success;
        p.setPen(QPen(bc, 3));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(1.5, 1.5, w - 3, h - 3));
        const QString text = paused ? tr("PAUSED") : sim ? tr("SIMULATING") : tr("PLAYING");
        QFont f = p.font();
        f.setPixelSize(11);
        f.setBold(true);
        f.setLetterSpacing(QFont::AbsoluteSpacing, 1.0);
        p.setFont(f);
        const QString full = text + QStringLiteral("  ·  %1 s").arg(m_ctx->play().elapsedSeconds(), 0, 'f', 1);
        const int tw = QFontMetrics(f).horizontalAdvance(full) + 28;
        QRectF pill(w / 2.0 - tw / 2.0, 10, tw, 24);
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(bc, 220));
        p.drawRoundedRect(pill, 12, 12);
        p.setPen(QColor(12, 14, 18));
        p.drawText(pill, Qt::AlignCenter, full);
    }
    // stats HUD
    if (m_flags.stats) {
        QStringList lines;
        lines << tr("%1 FPS  ·  %2 ms").arg(m_fps, 0, 'f', 0).arg(m_frameMs, 0, 'f', 2);
        if (m_painterRenderer) {
            const auto& s = m_painterRenderer->lastStats();
            lines << tr("Meshes %1  ·  Faces %2  ·  Lines %3").arg(s.entities).arg(s.faces).arg(s.lines);
        }
        lines << tr("Entities %1").arg(m_ctx->world().entityCount());
        lines << tr("Renderer: %1").arg(rendererName());
        QFont f = Theme::monoFont();
        p.setFont(f);
        const QFontMetrics fm(f);
        int tw = 0;
        for (const auto& l : lines) tw = std::max(tw, fm.horizontalAdvance(l));
        QRectF box(w - tw - 28, 10, tw + 18, lines.size() * (fm.height() + 2) + 12);
        p.setPen(QPen(c.overlayBorder, 1));
        p.setBrush(c.overlay);
        p.drawRoundedRect(box, 8, 8);
        p.setPen(c.text);
        for (int i = 0; i < lines.size(); ++i) p.drawText(QPointF(box.left() + 9, box.top() + 6 + fm.ascent() + i * (fm.height() + 2)), lines[i]);
    }
    // gizmo label
    if (m_gizmo.active() && !m_gizmo.dragLabel().isEmpty()) {
        QFont f = Theme::monoFont();
        p.setFont(f);
        const QString t = m_gizmo.dragLabel();
        const int tw = QFontMetrics(f).horizontalAdvance(t) + 16;
        QRectF box(m_lastPos.x() + 16, m_lastPos.y() + 12, tw, 22);
        p.setPen(QPen(c.overlayBorder, 1));
        p.setBrush(c.overlay);
        p.drawRoundedRect(box, 6, 6);
        p.setPen(c.text);
        p.drawText(box, Qt::AlignCenter, t);
    }
    // marquee
    if (m_selecting && m_marquee.isValid()) {
        p.setPen(QPen(c.accent, 1, Qt::DashLine));
        p.setBrush(withAlpha(c.accent, 30));
        p.drawRect(m_marquee);
    }
    p.restore();
}

QImage ViewportPanel::renderToImage(QSize size) {
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::black);
    QPainter p(&img);
    paintCanvas(p, size, true);
    return img;
}

void ViewportPanel::tick() {
    const double dt = std::min(0.1, double(m_clock.restart()) / 1000.0);
    // frame limiter / unfocused throttling
    const auto& prefs = m_ctx->preferences().values();
    const bool focused = window() && window()->isActiveWindow();
    const int fps = (!focused && prefs.throttleWhenUnfocused) ? std::max(1, prefs.unfocusedFps) : (prefs.frameLimit > 0 ? prefs.frameLimit : 240);
    const int interval = std::max(1, 1000 / fps);
    if (m_timer.interval() != interval) m_timer.setInterval(interval);

    if (m_flying && !m_keys.isEmpty()) {
        glm::vec3 move(0.0f);
        if (m_keys.contains(Qt::Key_W)) move += m_camera.forward();
        if (m_keys.contains(Qt::Key_S)) move -= m_camera.forward();
        if (m_keys.contains(Qt::Key_D)) move += m_camera.right();
        if (m_keys.contains(Qt::Key_A)) move -= m_camera.right();
        if (m_keys.contains(Qt::Key_E)) move += glm::vec3(0, 1, 0);
        if (m_keys.contains(Qt::Key_Q)) move -= glm::vec3(0, 1, 0);
        if (glm::length(move) > 0.0f) {
            const bool fast = QApplication::keyboardModifiers() & Qt::ShiftModifier;
            m_velocityBoost = std::min(4.0f, m_velocityBoost + float(dt * prefs.cameraAcceleration * 0.25));
            const float speed = float(prefs.cameraSpeed) * (fast ? 3.0f : 1.0f) * m_velocityBoost;
            m_camera.position += glm::normalize(move) * speed * float(dt);
            m_dirty = true;
        }
    } else {
        m_velocityBoost = 1.0f;
    }
    if (m_ctx->isPlaying()) m_dirty = true;
    if (m_dirty && isVisible()) {
        m_dirty = false;
#if OX_EDITOR_HAS_RHI
        if (m_vkWindow) m_vkWindow->renderFrame();
        else
#endif
            m_canvas->update();
    }
}

// ---- input --------------------------------------------------------------------------------------------------

void ViewportPanel::onMousePress(QMouseEvent* e) {
    m_surface->setFocus(Qt::MouseFocusReason);
    m_pressPos = m_lastPos = e->position();
    const bool alt = e->modifiers() & Qt::AltModifier;
    if (e->button() == Qt::RightButton) {
        if (alt) m_dollying = true;
        else {
            m_flying = true;
            setSurfaceCursor(Qt::BlankCursor);
        }
        return;
    }
    if (e->button() == Qt::MiddleButton) {
        m_panning = true;
        setSurfaceCursor(Qt::ClosedHandCursor);
        return;
    }
    if (e->button() != Qt::LeftButton) return;
    if (alt) {
        m_orbiting = true;
        m_orbitPivot = pivotPoint();
        return;
    }
    if (!m_ctx->isPlaying() || true) {
        const GizmoAxis axis = m_gizmo.hitTest(gizmoView(), e->position());
        if (axis != AxisNone && !m_ctx->isLocked(m_ctx->selection().primary())) {
            m_gizmo.begin(gizmoView(), e->position(), axis);
            m_gizmoDragging = true;
            m_dragStart.clear();
            World& w = m_ctx->world();
            for (const auto& id : topLevelOnly(w, m_ctx->selection().ids())) {
                Entity ent = w.find(id);
                if (!ent || m_ctx->isLocked(id)) continue;
                DragStart s;
                s.id = id;
                s.world = ent.worldTransform();
                s.parentWorld = ent.parent() ? ent.parent().worldMatrix() : glm::mat4(1.0f);
                m_dragStart.push_back(s);
            }
            // Alt-less duplicate-drag is Shift+drag in many editors; keep it simple: plain drag.
            applyGizmoDelta(TransformGizmo::Delta{}, EditPhase::Begin);
            return;
        }
    }
    m_selecting = true;
    m_marquee = QRectF();
}

void ViewportPanel::onMouseMove(QMouseEvent* e) {
    const QPointF pos = e->position();
    const QPointF d = pos - m_lastPos;
    m_lastPos = pos;
    const auto& prefs = m_ctx->preferences().values();
    const float sens = float(prefs.mouseSensitivity) * 0.0175f;
    const float invert = prefs.invertY ? -1.0f : 1.0f;
    if (m_flying) {
        m_camera.yaw -= float(d.x()) * sens;
        m_camera.pitch = std::clamp(m_camera.pitch - float(d.y()) * sens * invert, -1.55f, 1.55f);
        m_dirty = true;
        return;
    }
    if (m_orbiting) {
        const float dist = glm::length(m_camera.position - m_orbitPivot);
        m_camera.yaw -= float(d.x()) * sens;
        m_camera.pitch = std::clamp(m_camera.pitch - float(d.y()) * sens * invert, -1.55f, 1.55f);
        m_camera.position = m_orbitPivot - m_camera.forward() * dist;
        m_dirty = true;
        return;
    }
    if (m_panning) {
        const float dist = std::max(1.0f, glm::length(m_camera.position - pivotPoint()));
        const float k = dist * 0.0018f * (60.0f / std::max(10.0f, m_camera.verticalFovDeg)) * 0.9f;
        m_camera.position += (-m_camera.right() * float(d.x()) + m_camera.up() * float(d.y())) * k;
        m_dirty = true;
        return;
    }
    if (m_dollying) {
        m_camera.position += m_camera.forward() * float(-d.y()) * 0.05f * float(prefs.cameraSpeed);
        m_dirty = true;
        return;
    }
    if (m_gizmoDragging) {
        const auto delta = m_gizmo.update(gizmoView(), pos, e->modifiers() & Qt::ControlModifier);
        applyGizmoDelta(delta, EditPhase::Update);
        m_dirty = true;
        return;
    }
    if (m_selecting && (e->buttons() & Qt::LeftButton)) {
        if ((pos - m_pressPos).manhattanLength() > 4) {
            m_marquee = QRectF(m_pressPos, pos).normalized();
            m_dirty = true;
        }
        return;
    }
    const GizmoAxis hover = m_gizmo.hitTest(gizmoView(), pos);
    if (hover != m_gizmo.hover()) {
        m_gizmo.setHover(hover);
        setSurfaceCursor(hover != AxisNone ? Qt::PointingHandCursor : Qt::ArrowCursor);
        m_dirty = true;
    }
}

void ViewportPanel::onMouseRelease(QMouseEvent* e) {
    if (e->button() == Qt::RightButton) {
        m_flying = false;
        m_dollying = false;
        unsetSurfaceCursor();
        return;
    }
    if (e->button() == Qt::MiddleButton) {
        m_panning = false;
        unsetSurfaceCursor();
        return;
    }
    if (e->button() != Qt::LeftButton) return;
    if (m_orbiting) {
        m_orbiting = false;
        return;
    }
    if (m_gizmoDragging) {
        const auto delta = m_gizmo.update(gizmoView(), e->position(), e->modifiers() & Qt::ControlModifier);
        applyGizmoDelta(delta, EditPhase::End);
        m_gizmo.end();
        m_gizmoDragging = false;
        m_dragStart.clear();
        m_dirty = true;
        return;
    }
    if (m_selecting) {
        m_selecting = false;
        if (m_marquee.isValid() && m_marquee.width() > 4 && m_marquee.height() > 4) {
            const auto locked = [&] {
                UuidList l;
                World& w = m_ctx->world();
                w.forEachInHierarchy([&](entt::entity h) {
                    const Uuid id = w.wrap(h).uuid();
                    if (m_ctx->isLocked(id)) l.push_back(id);
                });
                return l;
            }();
            UuidList ids = pickRect(m_ctx->world(), gizmoView(), m_marquee, m_ctx->hiddenIds(), locked);
            if (e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier)) {
                for (const auto& id : ids) m_ctx->selection().add(id);
            } else {
                m_ctx->selection().set(ids);
            }
            m_marquee = QRectF();
            m_dirty = true;
            return;
        }
        m_marquee = QRectF();
        pickAt(e->position(), e->modifiers());
    }
}

void ViewportPanel::pickAt(QPointF pos, Qt::KeyboardModifiers mods) {
    World& w = m_ctx->world();
    UuidList locked;
    w.forEachInHierarchy([&](entt::entity h) {
        const Uuid id = w.wrap(h).uuid();
        if (m_ctx->isLocked(id)) locked.push_back(id);
    });
    Uuid hit;
    const ViewportFrame frame = makeFrame(m_surface->size());
    if (!m_renderer || !m_renderer->pick(frame, glm::ivec2(int(pos.x()), int(pos.y())), hit)) {
        if (auto h = pickEntity(w, gizmoView(), pos, m_ctx->hiddenIds(), locked)) hit = h->id;
    }
    if (mods & (Qt::ControlModifier | Qt::MetaModifier)) {
        if (!hit.isNil()) m_ctx->selection().toggle(hit);
    } else if (mods & Qt::ShiftModifier) {
        if (!hit.isNil()) m_ctx->selection().add(hit);
    } else {
        m_ctx->selection().select(hit);
    }
}

void ViewportPanel::applyGizmoDelta(const TransformGizmo::Delta& d, EditPhase phase) {
    if (m_dragStart.empty()) return;
    const glm::vec3 pivot0 = m_dragStart.back().world.position;
    UuidList ids;
    std::vector<serial::Value> values;
    for (const DragStart& s : m_dragStart) {
        Transform t = s.world;
        switch (m_gizmo.mode()) {
        case GizmoMode::Translate: t.position += d.translation; break;
        case GizmoMode::Rotate:
            t.rotation = glm::normalize(d.rotation * s.world.rotation);
            t.position = pivot0 + d.rotation * (s.world.position - pivot0);
            break;
        case GizmoMode::Scale:
            t.scale = s.world.scale * d.scale;
            if (m_dragStart.size() > 1) t.position = pivot0 + (s.world.position - pivot0) * d.scale;
            break;
        default: break;
        }
        const Transform local = Transform::fromMatrix(glm::inverse(s.parentWorld) * t.toMatrix());
        TransformComponent tc;
        tc.setTransform(local);
        ids.push_back(s.id);
        values.push_back(serial::toValue(tc));
    }
    m_ctx->setProperty(ids, "Transform", "", values, phase);
}

void ViewportPanel::onWheel(QWheelEvent* e) {
    const double steps = e->angleDelta().y() / 120.0;
    if (m_flying) {
        const double speed = std::clamp(m_ctx->preferences().values().cameraSpeed * std::pow(1.2, steps), 0.1, 200.0);
        m_ctx->preferences().modify([speed](PreferenceValues& v) { v.cameraSpeed = speed; });
        Q_EMIT cameraSpeedChanged(speed);
        return;
    }
    if (m_camera.orthographic) {
        m_camera.orthoHeight = std::clamp(m_camera.orthoHeight * float(std::pow(0.88, steps)), 0.1f, 5000.0f);
    } else {
        const float dist = std::max(0.5f, glm::length(m_camera.position - pivotPoint()));
        m_camera.position += m_camera.forward() * float(steps) * dist * 0.12f;
    }
    m_dirty = true;
}

void ViewportPanel::onKey(QKeyEvent* e, bool down) {
    if (e->isAutoRepeat()) {
        e->accept();
        return;
    }
    const int key = e->key();
    if (m_flying) {
        if (down) m_keys.insert(key);
        else m_keys.remove(key);
        e->accept();
        return;
    }
    if (!down) {
        m_keys.remove(key);
        return;
    }
    if (e->modifiers() & (Qt::ControlModifier | Qt::MetaModifier | Qt::AltModifier)) {
        e->ignore();
        return;
    }
    switch (key) {
    case Qt::Key_Q: setGizmoMode(GizmoMode::Select); break;
    case Qt::Key_W: setGizmoMode(GizmoMode::Translate); break;
    case Qt::Key_E: setGizmoMode(GizmoMode::Rotate); break;
    case Qt::Key_R: setGizmoMode(GizmoMode::Scale); break;
    case Qt::Key_F: focusSelection(); break;
    case Qt::Key_Escape:
        if (m_gizmoDragging) {
            m_gizmo.end();
            m_gizmoDragging = false;
            applyGizmoDelta(TransformGizmo::Delta{}, EditPhase::End);
            m_dragStart.clear();
        } else {
            m_ctx->selection().clear();
        }
        break;
    default: e->ignore(); return;
    }
    e->accept();
    m_dirty = true;
}

void ViewportPanel::dropAsset(const QString& line, QPointF pos) {
    const QStringList parts = line.split(QLatin1Char('|'));
    if (parts.size() < 3) return;
    const GizmoView gv = gizmoView();
    const Ray r = gv.ray(pos);
    glm::vec3 at = r.origin + r.direction * 8.0f;
    if (auto t = intersectRayPlane(r, Plane{{0, 1, 0}, 0.0f}); t && *t > 0 && *t < 500) at = r.at(*t);
    const QString type = parts[1];
    if (type == QLatin1String("Prefab")) {
        m_ctx->instantiatePrefab(parts[2], {}, at);
    } else if (type == QLatin1String("Mesh")) {
        auto id = Uuid::parse(parts[0].toStdString());
        if (!id) return;
        const QString name = QFileInfo(parts[2]).completeBaseName();
        const Uuid meshId = *id;
        auto ids = m_ctx->createEntities(tr("Place %1").arg(name), [name, meshId, at](World& w) {
            Entity e = w.create(name.toStdString());
            e.setPosition(at);
            auto& mr = e.add<MeshRendererComponent>();
            mr.mesh = meshId;
            return std::vector<Entity>{e};
        });
        if (!ids.empty()) m_ctx->selection().select(ids.front());
    } else if (type == QLatin1String("Scene")) {
        Q_EMIT m_ctx->statusMessage(tr("Open scenes from the Content Browser (double-click)"), 3000);
    }
}

} // namespace ox::editor
