#include "viewport/viewport_panel.hpp"

#include "core/editor_context.hpp"
#include "integration/gameplay_tools.hpp"
#include "integration/input_bridge.hpp"
#include "inspector/property_editors.hpp"
#include "theme/icons.hpp"
#include "theme/theme.hpp"
#include "widgets/widgets.hpp"

#include <oxwald/core/serial/convert.hpp>
#include <oxwald/scene/component_registry.hpp>
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

#include <QCursor>
#include <QMessageBox>

#if OX_EDITOR_HAS_RHI
#include <oxwald/rhi/device.hpp>
#endif

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
    m_panel->clearInputState();
    QWidget::focusOutEvent(e);
}
void ViewportCanvas::resizeEvent(QResizeEvent* e) {
    m_panel->requestRedraw();
    QWidget::resizeEvent(e);
}
bool ViewportCanvas::event(QEvent* e) {
    // While flying (or playing with captured input), keys drive the camera/game instead of tool shortcuts.
    if (e->type() == QEvent::ShortcutOverride && m_panel->isFlying()) {
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
    // The software renderer always exists (fallback, headless); the GPU renderer is created once the viewport's
    // Vulkan device exists (onDeviceReady).
    m_painterRenderer = std::make_unique<PainterViewportRenderer>();
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
        maybeCreateOffscreenGpu();
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
    connect(&ctx->tools(), &ToolState::changed, this, redraw);
    connect(&ctx->runtime(), &RuntimeHost::ticked, this, redraw);
    connect(ctx, &EditorContext::playStateChanged, this, [this] {
        if (!m_ctx->isPlaying()) setInputCaptured(false);
    });
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

ViewportPanel::~ViewportPanel() { releaseGpuRenderer(); }

float ViewportPanel::viewEv100() const {
    if (m_gameExposure) {
        World& w = m_ctx->world();
        for (auto [h, cam] : w.view<CameraComponent>().each()) {
            if (cam.primary) return cam.ev100();
        }
    }
    return m_manualEv;
}

IViewportRenderer* ViewportPanel::renderer() const {
    if (m_gpuRenderer) return m_gpuRenderer.get();
    return m_painterRenderer.get();
}

void ViewportPanel::onDeviceReady(rhi::Device& device) {
    if (m_gpuRenderer) return;
    if (const auto& factory = m_ctx->services().viewportRendererFactory()) {
        m_gpuRenderer = factory(device);
        if (m_gpuRenderer) OX_LOG_INFO("editor", "Viewport renderer: {}", m_gpuRenderer->name().toStdString());
    }
    m_dirty = true;
}

void ViewportPanel::releaseGpuRenderer() {
#if OX_EDITOR_HAS_RHI
    if (m_gpuRenderer) {
        if (rhi::Device* d = VulkanViewportHub::device()) d->waitIdle();
    }
    m_gpuRenderer.reset();
    if (m_headlessDevice) {
        VulkanViewportHub::setDevice(nullptr);
        m_headlessDevice.reset();
    }
#else
    m_gpuRenderer.reset();
#endif
}

// Headless GPU canvas: with OX_EDITOR_OFFSCREEN_GPU=1 (screenshots) a surface-less device renders every frame
// offscreen and the canvas paints the image + overlays.
void ViewportPanel::maybeCreateOffscreenGpu() {
#if OX_EDITOR_HAS_RHI
    if (!qEnvironmentVariableIsSet("OX_EDITOR_OFFSCREEN_GPU") || qEnvironmentVariableIsSet("OX_EDITOR_NO_VULKAN")) return;
    if (!m_ctx->services().viewportRendererFactory() || VulkanViewportHub::device()) return;
    rhi::DeviceDesc desc;
    desc.appName = "OxwaldEditor offscreen";
    desc.framesInFlight = 2;
    desc.validation = false;
    VulkanViewportHub::prepareDeviceDesc(desc);
    std::string err;
    std::unique_ptr<rhi::Device> device = rhi::Device::create(desc, &err);
    if (!device) {
        OX_LOG_WARN("editor", "offscreen GPU canvas unavailable: {}", err);
        return;
    }
    rhi::Device* raw = device.get();
    m_headlessDevice = std::shared_ptr<void>(device.release(), [](void* d) { delete static_cast<rhi::Device*>(d); });
    VulkanViewportHub::setDevice(raw);
    onDeviceReady(*raw);
#endif
}

void ViewportPanel::clearInputState() {
    m_keys.clear();
    m_flying = false;
    m_sculpting = false;
    setInputCaptured(false);
}

void ViewportPanel::setInputCaptured(bool captured) {
    if (captured == m_captured) return;
    m_captured = captured;
    if (captured) {
        setSurfaceCursor(Qt::BlankCursor);
        m_captureCenter = QPointF(m_surface->width() / 2.0, m_surface->height() / 2.0);
        m_ignoreNextMove = true;
        QCursor::setPos(m_surface->mapToGlobal(m_captureCenter.toPoint()));
        Q_EMIT m_ctx->statusMessage(tr("Game input captured — Shift+F1 releases the mouse, Esc stops playing"), 4000);
    } else {
        unsetSurfaceCursor();
        forwardFocusLost(m_ctx->engine());
    }
    m_dirty = true;
}

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
    releaseGpuRenderer();
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
    QElapsedTimer t;
    t.start();
    World& world = m_ctx->world();
    world.updateTransforms();
    DebugDraw& dd = m_ctx->debugDraw();
    updateGizmoTarget();
    // gizmo, grid, light/camera shapes and gameplay debug draw go to the renderer as lines
    const qreal dpr = devicePixelRatioF();
    const QSize logical(int(sizePx.width() / dpr), int(sizePx.height() / dpr));
    const GizmoView gv = GizmoView::from(m_camera, glm::vec2(logical.width(), logical.height()));
    if (!m_ctx->isPlaying()) m_gizmo.emitLines(dd, gv);
    submitOverlayLines(dd, true, logical);
    dd.flush(0.016f);
    ViewportFrame frame = makeFrame(sizePx);
    const UuidList hidden = m_ctx->hiddenIds();
    frame.hidden = hidden;
    frame.lines = &dd;
    frame.time = m_clock.elapsed() / 1000.0;
    m_gpuRenderer->render(frame, target);
    m_frameMs = double(t.nsecsElapsed()) / 1e6;
    ++m_fpsFrames;
    if (m_fpsClock.elapsed() >= 500) {
        m_fps = m_fpsFrames * 1000.0 / double(m_fpsClock.restart());
        m_fpsFrames = 0;
    }
    Q_EMIT frameRendered(m_frameMs);
}

QString ViewportPanel::rendererName() const { return renderer() ? renderer()->name() : QString(); }

void ViewportPanel::submitOverlayLines(DebugDraw& dd, bool gpuFrame, QSize size) {
    (void)size;
    World& world = m_ctx->world();
    applyGameplayDebugFlags(m_ctx->engineServices(), m_flags);
    if (m_flags.bounds) {
        world.forEachInHierarchy([&](entt::entity h) {
            Entity e = world.wrap(h);
            if (e.has<MeshRendererComponent>()) dd.aabb(entityBounds(world, e), glm::vec4(0.3f, 0.9f, 0.9f, 0.6f));
        });
    }
    if (gpuFrame) {
        // The grid is drawn by the renderer (EditorViewportFrame::grid). Light and camera shapes (the software renderer draws them itself, with icons).
        const UuidList sel = m_ctx->selection().ids();
        auto selected = [&](Entity e) { return std::find(sel.begin(), sel.end(), e.uuid()) != sel.end(); };
        if (m_flags.lights) {
            for (auto [h, l] : world.view<LightComponent>().each()) {
                Entity e = world.wrap(h);
                const glm::vec3 p = e.worldPosition();
                const glm::vec3 fwd = e.worldRotation() * glm::vec3(0, 0, -1);
                const DebugColor c = selected(e) ? DebugColor(debug_color::kOrange) : DebugColor(glm::vec4(1.0f, 0.9f, 0.6f, 0.8f));
                switch (l.type) {
                case LightType::Directional: dd.arrow(p, p + fwd * 1.5f, 0.25f, c, 0.0f, false); break;
                case LightType::Spot:
                    dd.cone(p, fwd, selected(e) ? l.range : 1.0f, glm::radians(l.outerConeAngle), c, 0.0f, true, 16);
                    break;
                case LightType::Point:
                    dd.sphere(p, selected(e) ? l.range : 0.25f, c, 0.0f, true, selected(e) ? 32 : 12);
                    break;
                default: dd.box(p, glm::vec3(l.areaSize * 0.5f, 0.01f), e.worldRotation(), c); break;
                }
            }
        }
        if (m_flags.cameras) {
            for (auto [h, cam] : world.view<CameraComponent>().each()) {
                Entity e = world.wrap(h);
                const glm::mat4 m = e.worldMatrix();
                const DebugColor c = selected(e) ? DebugColor(debug_color::kOrange) : DebugColor(glm::vec4(0.85f, 0.88f, 0.95f, 0.8f));
                const glm::vec3 o = glm::vec3(m[3]);
                const float s = 0.35f;
                const glm::vec3 corners[4] = {glm::vec3(m * glm::vec4(-s, -s * 0.6f, -s * 1.4f, 1)), glm::vec3(m * glm::vec4(s, -s * 0.6f, -s * 1.4f, 1)),
                                              glm::vec3(m * glm::vec4(s, s * 0.6f, -s * 1.4f, 1)), glm::vec3(m * glm::vec4(-s, s * 0.6f, -s * 1.4f, 1))};
                for (int i = 0; i < 4; ++i) {
                    dd.line(o, corners[i], c);
                    dd.line(corners[i], corners[(i + 1) % 4], c);
                }
            }
        }
    }
    // Gameplay debug draw of the last engine frame (colliders, splines, navmesh, contacts, ...).
    if (m_flags.debugDraw) {
        if (const DebugDraw* game = m_ctx->gameDebugDraw()) {
            const auto copy = [&](std::span<const DebugVertex> v, bool depth) {
                for (usize i = 0; i + 1 < v.size(); i += 2) dd.line(v[i].position, v[i + 1].position, DebugColor(v[i].color), 0.0f, depth);
            };
            copy(game->depthTestedLines(), true);
            copy(game->overlayLines(), false);
        }
    }
    // Tool overlays.
    ToolState& tools = m_ctx->tools();
    if (tools.tool() == ViewportTool::SplinePoints) {
        drawSplineHandles(dd, world, tools.target(), tools.splinePoint, m_camera.position);
    } else if (tools.tool() == ViewportTool::TerrainSculpt) {
        drawTerrainWire(dd, m_ctx->engineServices(), world, tools.target(), m_brushPos, tools.sculpt.radius);
    }
}

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
    vm->addSection(tr("Exposure"));
    auto* gameEv = vm->addAction(tr("Game Camera Exposure"));
    gameEv->setCheckable(true);
    gameEv->setChecked(m_gameExposure);
    gameEv->setToolTip(tr("Use the EV100 of the scene's primary camera (aperture / shutter / ISO)"));
    connect(gameEv, &QAction::toggled, this, [this](bool on) {
        m_gameExposure = on;
        m_dirty = true;
    });
    auto* evAction = new QWidgetAction(vm);
    auto* ev = new NumberField(vm);
    ev->setRange(-6, 24);
    ev->setStep(0.1);
    ev->setDecimals(1);
    ev->setAxis(QStringLiteral("EV"), colors().warning);
    ev->setValue(m_manualEv);
    ev->setMinimumWidth(160);
    ev->setToolTip(tr("Manual exposure (EV100) of the editor view"));
    connect(ev, &NumberField::edited, this, [this, gameEv](double v, EditPhase) {
        m_manualEv = float(v);
        gameEv->setChecked(false);
        m_dirty = true;
    });
    evAction->setDefaultWidget(ev);
    vm->addAction(evAction);
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
        if (!gameplayAvailable()) return;
        sm->addSection(tr("Gameplay Debug"));
        for (const F& f : {F{tr("Physics Colliders"), &ShowFlags::physicsColliders}, F{tr("Physics Contacts"), &ShowFlags::physicsContacts},
                           F{tr("Navigation Mesh"), &ShowFlags::navigation}, F{tr("Splines"), &ShowFlags::splines},
                           F{tr("Skeletons"), &ShowFlags::skeletons}, F{tr("Audio Sources"), &ShowFlags::audio}}) {
            QAction* a = sm->addAction(f.name);
            a->setCheckable(true);
            a->setChecked(m_flags.*(f.flag));
            a->setObjectName(QStringLiteral("show:") + f.name);
            auto member = f.flag;
            connect(a, &QAction::toggled, this, [this, member](bool on) {
                m_flags.*member = on;
                m_dirty = true;
            });
        }
        sm->addSeparator();
        sm->addAction(Icons::get(QStringLiteral("map")), tr("Bake Navigation Mesh"), this, [this] {
            const BakeResult r = bakeNavMesh(*m_ctx);
            Q_EMIT m_ctx->statusMessage(r.message, 5000);
            m_flags.navigation = true;
            m_dirty = true;
        })->setEnabled(!m_ctx->isPlaying());
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
    ToolState& tools = m_ctx->tools();
    if (tools.tool() == ViewportTool::TerrainSculpt) {
        m_gizmo.setVisible(false);
        return;
    }
    if (tools.tool() == ViewportTool::SplinePoints) {
        const auto pts = splinePointsWorld(w, tools.target());
        const bool visible = tools.splinePoint >= 0 && tools.splinePoint < int(pts.size()) && m_flags.gizmos;
        m_gizmo.setVisible(visible);
        if (visible && !m_gizmo.active()) {
            if (m_gizmo.mode() != GizmoMode::Translate) m_gizmo.setMode(GizmoMode::Translate);
            m_gizmo.setTarget(pts[usize(tools.splinePoint)], glm::quat(1, 0, 0, 0));
        }
        return;
    }
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
    f.ev100 = viewEv100();
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
    const bool gpu = m_gpuRenderer != nullptr; // headless GPU canvas: offscreen frame + painter overlays
    submitOverlayLines(dd, gpu, size);
    dd.flush(0.016f);
    ViewportFrame frame = makeFrame(size);
    const UuidList hidden = m_ctx->hiddenIds();
    frame.hidden = hidden;
    frame.lines = &dd;
    frame.time = m_clock.elapsed() / 1000.0;
    QImage gpuImage;
    if (gpu) {
        const qreal dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
        const QSize px(int(size.width() * dpr), int(size.height() * dpr));
        ViewportFrame f = frame;
        f.sizePx = glm::uvec2(std::max(1, px.width()), std::max(1, px.height()));
        f.projection = m_camera.projection(float(f.sizePx.x) / float(f.sizePx.y));
        f.viewProjection = f.projection * f.view;
        gpuImage = m_gpuRenderer->renderOffscreen(f, px);
    }
    if (!gpuImage.isNull()) {
        p.drawImage(QRectF(0, 0, size.width(), size.height()), gpuImage);
    } else if (m_painterRenderer) {
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
    if (m_gpuRenderer && !m_canvas) {
        // Vulkan window: render the same view offscreen (outside the viewport's device frame) + painter overlays.
        World& world = m_ctx->world();
        world.updateTransforms();
        DebugDraw& dd = m_ctx->debugDraw();
        submitOverlayLines(dd, true, size);
        dd.flush(0.0f);
        ViewportFrame frame = makeFrame(size);
        const UuidList hidden = m_ctx->hiddenIds();
        frame.hidden = hidden;
        frame.lines = &dd;
        const QImage gpu = m_gpuRenderer->renderOffscreen(frame, size);
        if (!gpu.isNull()) {
            p.drawImage(QPoint(0, 0), gpu);
            updateGizmoTarget();
            m_gizmo.draw(p, GizmoView::from(m_camera, glm::vec2(size.width(), size.height())));
            paintOverlay(p, size);
            return img;
        }
    }
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

    if (m_flying && !m_captured && !m_keys.isEmpty()) {
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
    tickTools(dt);
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
    if (m_captured) {
        forwardMouseButton(m_ctx->engine(), e->button(), true);
        return;
    }
    // Play-in-editor: clicking into the viewport possesses the game (input capture) like UE.
    if (m_ctx->isPlaying() && m_ctx->play().mode() == PlayMode::Play && m_ctx->engine() && e->button() == Qt::LeftButton && !alt) {
        setInputCaptured(true);
        forwardMouseButton(m_ctx->engine(), e->button(), true);
        return;
    }
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
    if (handleToolPress(e)) return;
    if (!m_ctx->isPlaying() || true) {
        const GizmoAxis axis = m_gizmo.hitTest(gizmoView(), e->position());
        if (axis != AxisNone && !m_ctx->isLocked(m_ctx->selection().primary())) {
            m_gizmo.begin(gizmoView(), e->position(), axis);
            m_gizmoDragging = true;
            m_dragStart.clear();
            if (m_ctx->tools().tool() == ViewportTool::SplinePoints && m_ctx->tools().splinePoint >= 0) {
                const auto pts = splinePointsWorld(m_ctx->world(), m_ctx->tools().target());
                if (m_ctx->tools().splinePoint < int(pts.size())) m_splineDragStart = pts[usize(m_ctx->tools().splinePoint)];
                DragStart s;
                s.id = m_ctx->tools().target();
                m_dragStart.push_back(s);
                applyGizmoDelta(TransformGizmo::Delta{}, EditPhase::Begin);
                return;
            }
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
    if (m_captured) {
        const QPointF delta = pos - m_captureCenter;
        if (m_ignoreNextMove || (std::abs(delta.x()) < 0.5 && std::abs(delta.y()) < 0.5)) {
            m_ignoreNextMove = false;
            return;
        }
        forwardMouseMove(m_ctx->engine(), pos, delta);
        m_ignoreNextMove = true;
        QCursor::setPos(m_surface->mapToGlobal(m_captureCenter.toPoint()));
        return;
    }
    const QPointF d = pos - m_lastPos;
    m_lastPos = pos;
    if (m_ctx->tools().tool() == ViewportTool::TerrainSculpt && !m_flying && !m_orbiting && !m_panning) {
        updateSculptBrush(pos);
        m_dirty = true;
    }
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
    if (m_captured) {
        forwardMouseButton(m_ctx->engine(), e->button(), false);
        return;
    }
    if (m_sculpting && e->button() == Qt::LeftButton) {
        m_sculpting = false;
        m_ctx->markDirty();
        return;
    }
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
    IViewportRenderer* r = renderer();
    const qreal dpr = devicePixelRatioF();
    ViewportFrame pf = frame;
    if (m_gpuRenderer) {
        // ID-buffer pick in device pixels.
        pf = makeFrame(QSize(int(m_surface->width() * dpr), int(m_surface->height() * dpr)));
    }
    const glm::ivec2 px = m_gpuRenderer ? glm::ivec2(int(pos.x() * dpr), int(pos.y() * dpr)) : glm::ivec2(int(pos.x()), int(pos.y()));
    if (!r || !r->pick(pf, px, hit)) {
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
    if (m_ctx->tools().tool() == ViewportTool::SplinePoints && m_ctx->tools().splinePoint >= 0) {
        setSplinePointWorld(*m_ctx, m_ctx->tools().target(), m_ctx->tools().splinePoint, m_splineDragStart + d.translation, phase);
        return;
    }
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
    if (m_captured) {
        forwardWheel(m_ctx->engine(), QPointF(e->angleDelta()) / 8.0);
        return;
    }
    if (m_ctx->tools().tool() == ViewportTool::TerrainSculpt && (e->modifiers() & Qt::ControlModifier)) {
        auto& r = m_ctx->tools().sculpt.radius;
        r = std::clamp(r * float(std::pow(1.15, e->angleDelta().y() / 120.0)), 0.5f, 200.0f);
        m_dirty = true;
        return;
    }
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
    const int key = e->key();
    if (m_ctx->isPlaying() && down) {
        // UE conventions: Shift+F1 releases the mouse, Esc stops play-in-editor.
        if (key == Qt::Key_F1 && (e->modifiers() & Qt::ShiftModifier)) {
            setInputCaptured(false);
            e->accept();
            return;
        }
        if (key == Qt::Key_Escape) {
            setInputCaptured(false);
            m_ctx->stopPlay();
            e->accept();
            return;
        }
    }
    if (m_captured) {
        forwardKey(m_ctx->engine(), e, down);
        e->accept();
        return;
    }
    if (e->isAutoRepeat()) {
        e->accept();
        return;
    }
    if (down && m_ctx->tools().tool() == ViewportTool::SplinePoints && (key == Qt::Key_Delete || key == Qt::Key_Backspace)) {
        ToolState& t = m_ctx->tools();
        if (t.splinePoint >= 0) {
            removeSplinePoint(*m_ctx, t.target(), t.splinePoint);
            t.splinePoint = std::min(t.splinePoint, int(splinePointsWorld(m_ctx->world(), t.target()).size()) - 1);
            m_dirty = true;
        }
        e->accept();
        return;
    }
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
        if (m_ctx->tools().tool() != ViewportTool::Transform) {
            m_ctx->tools().reset();
        } else if (m_gizmoDragging) {
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
    if (type == QLatin1String("Prefab") || type == QLatin1String("Model")) {
        m_ctx->instantiatePrefab(parts[2], {}, at);
    } else if (type == QLatin1String("Material") || type == QLatin1String("Script")) {
        // Applied to the entity under the cursor.
        auto id = Uuid::parse(parts[0].toStdString());
        const Uuid target = entityUnder(pos);
        Entity e = m_ctx->world().find(target);
        if (!id || !e) {
            Q_EMIT m_ctx->statusMessage(tr("Drop the %1 onto an object").arg(type.toLower()), 3000);
            return;
        }
        if (type == QLatin1String("Material")) {
            if (!e.has<MeshRendererComponent>()) return;
            auto mats = e.get<MeshRendererComponent>().materials;
            if (mats.empty()) mats.push_back(*id);
            else mats[0] = *id;
            m_ctx->setProperty({target}, "MeshRenderer", "materials", serial::toValue(mats));
        } else {
            const ComponentInfo* script = ComponentRegistry::instance().find("Script");
            if (!script) return;
            if (!script->has(m_ctx->world(), e.handle())) m_ctx->addComponent({target}, "Script");
            m_ctx->setProperty({target}, "Script", "asset", serial::Value::makeUuid(*id));
        }
        m_ctx->selection().select(target);
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

namespace ox::editor {

bool ViewportPanel::handleToolPress(QMouseEvent* e) {
    ToolState& t = m_ctx->tools();
    if (e->button() != Qt::LeftButton || t.tool() == ViewportTool::Transform) return false;
    World& w = m_ctx->world();
    const GizmoView gv = gizmoView();
    const QPointF pos = e->position();
    if (t.tool() == ViewportTool::SplinePoints) {
        if (t.splinePoint >= 0 && m_gizmo.hitTest(gv, pos) != AxisNone) return false; // gizmo drag
        const auto pts = splinePointsWorld(w, t.target());
        int best = -1;
        double bestD = 12.0;
        for (usize i = 0; i < pts.size(); ++i) {
            QPointF sp;
            if (!gv.project(pts[i], sp)) continue;
            const double d = std::hypot(sp.x() - pos.x(), sp.y() - pos.y());
            if (d < bestD) {
                bestD = d;
                best = int(i);
            }
        }
        if (best < 0 && (e->modifiers() & (Qt::ControlModifier | Qt::MetaModifier))) {
            // Ctrl+click adds a point on the plane through the selected (or last) point.
            const Ray r = gv.ray(pos);
            const float planeY = pts.empty() ? 0.0f : pts[usize(t.splinePoint >= 0 ? t.splinePoint : int(pts.size()) - 1)].y;
            glm::vec3 at = r.origin + r.direction * 8.0f;
            if (auto hit = intersectRayPlane(r, Plane{{0, 1, 0}, -planeY}); hit && *hit > 0 && *hit < 2000) at = r.at(*hit);
            best = insertSplinePoint(*m_ctx, t.target(), t.splinePoint >= 0 ? t.splinePoint : int(pts.size()) - 1, at);
        }
        t.splinePoint = best;
        m_dirty = true;
        return true;
    }
    if (t.tool() == ViewportTool::TerrainSculpt) {
        updateSculptBrush(pos);
        m_sculpting = m_brushPos.has_value();
        m_dirty = true;
        return true;
    }
    return false;
}

void ViewportPanel::updateSculptBrush(QPointF pos) {
    Uuid terrain;
    m_brushPos = raycastTerrain(m_ctx->engineServices(), m_ctx->world(), gizmoView().ray(pos), &terrain);
    m_brushTerrain = terrain;
}

void ViewportPanel::tickTools(double dt) {
    ToolState& t = m_ctx->tools();
    if (t.tool() != ViewportTool::TerrainSculpt || !m_sculpting) return;
    updateSculptBrush(m_lastPos);
    if (!m_brushPos) return;
    SculptSettings s = t.sculpt;
    if (QApplication::keyboardModifiers() & Qt::ShiftModifier) s.op = SculptOp::Smooth;
    else if ((QApplication::keyboardModifiers() & Qt::ControlModifier) && s.op == SculptOp::Raise) s.op = SculptOp::Lower;
    const Uuid terrain = m_brushTerrain.isNil() ? t.target() : m_brushTerrain;
    if (sculptTerrain(m_ctx->engineServices(), m_ctx->world(), terrain, {m_brushPos->x, m_brushPos->z}, s, float(dt))) m_dirty = true;
}

Uuid ViewportPanel::entityUnder(QPointF pos) {
    World& w = m_ctx->world();
    Uuid hit;
    IViewportRenderer* r = renderer();
    const qreal dpr = devicePixelRatioF();
    if (m_gpuRenderer) {
        const ViewportFrame f = makeFrame(QSize(int(m_surface->width() * dpr), int(m_surface->height() * dpr)));
        if (r->pick(f, glm::ivec2(int(pos.x() * dpr), int(pos.y() * dpr)), hit)) return hit;
    }
    if (auto h = pickEntity(w, gizmoView(), pos, m_ctx->hiddenIds(), {})) hit = h->id;
    return hit;
}

} // namespace ox::editor
