#pragma once

#include "core/common.hpp"
#include "viewport/gizmo.hpp"
#include "viewport/painter_renderer.hpp"
#include "viewport/viewport_renderer.hpp"

#include <oxwald/core/serial/value.hpp>

#include <QElapsedTimer>
#include <QSet>
#include <QTimer>
#include <QWidget>

#include <memory>
#include <optional>

class QLabel;
class QToolButton;
class QMenu;

namespace ox::rhi {
class Device;
}

namespace ox::editor {

class EditorContext;
class ViewportPanel;
class VulkanViewportWindow;

// The surface the software renderer paints into (and that receives viewport input).
class ViewportCanvas : public QWidget {
    Q_OBJECT
public:
    explicit ViewportCanvas(ViewportPanel* panel);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;
    void focusOutEvent(QFocusEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dropEvent(QDropEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    bool event(QEvent* e) override;

private:
    ViewportPanel* m_panel;
};

class ViewportPanel : public QWidget {
    Q_OBJECT
public:
    explicit ViewportPanel(EditorContext* ctx, QWidget* parent = nullptr);
    ~ViewportPanel() override;

    [[nodiscard]] ViewportCamera& camera() { return m_camera; }
    [[nodiscard]] TransformGizmo& gizmo() { return m_gizmo; }
    void setGizmoMode(GizmoMode mode);
    void setGizmoSpace(GizmoSpace space);
    void setSnapEnabled(bool enabled);
    [[nodiscard]] bool snapEnabled() const { return m_gizmo.snap().enabled; }
    [[nodiscard]] ViewMode viewMode() const { return m_viewMode; }
    void setViewMode(ViewMode mode);
    [[nodiscard]] ShowFlags& showFlags() { return m_flags; }
    void setShowStats(bool on);
    void focusSelection();
    // Renders the current view into an image (screenshots, project thumbnails).
    [[nodiscard]] QImage renderToImage(QSize size);
    [[nodiscard]] double fps() const { return m_fps; }
    [[nodiscard]] double frameMs() const { return m_frameMs; }
    [[nodiscard]] QString rendererName() const;
    // The active renderer: the GPU one (render module) when a device exists, else the software renderer.
    [[nodiscard]] IViewportRenderer* renderer() const;
    [[nodiscard]] IViewportRenderer* gpuRenderer() const { return m_gpuRenderer.get(); }
    // The Vulkan device of the viewport exists: create the GPU renderer through EditorServices' factory.
    void onDeviceReady(rhi::Device& device);
    // The device is about to be destroyed.
    void releaseGpuRenderer();
    void requestRedraw() { m_dirty = true; }
    [[nodiscard]] bool usingVulkan() const { return m_vkWindow != nullptr; }
    [[nodiscard]] bool isFlying() const { return m_flying || m_captured; }
    void clearInputState();
    // Play-in-editor input capture (mouse locked to the viewport, keys/mouse forwarded to the InputSystem).
    [[nodiscard]] bool inputCaptured() const { return m_captured; }
    void setInputCaptured(bool captured);
    // GPU renderer path (render module): fills the frame description and calls IViewportRenderer::render.
    void renderGpuFrame(const ViewportTarget& target, QSize sizePx);

Q_SIGNALS:
    void gizmoModeChanged(ox::editor::GizmoMode mode);
    void gizmoSpaceChanged(ox::editor::GizmoSpace space);
    void snapChanged(bool on);
    void cameraSpeedChanged(double speed);
    void frameRendered(double frameMs);

private:
    friend class ViewportCanvas;
    friend class VulkanViewportWindow;
    void buildToolbar();
    void tick();
    ViewportFrame makeFrame(QSize size);
    void paintCanvas(QPainter& p, QSize size, bool forScreenshot = false);
    void paintOverlay(QPainter& p, QSize size);
    void updateGizmoTarget();
    [[nodiscard]] GizmoView gizmoView() const;
    void applyGizmoDelta(const TransformGizmo::Delta& d, EditPhase phase);
    void pickAt(QPointF pos, Qt::KeyboardModifiers mods);
    glm::vec3 pivotPoint() const;
    void dropAsset(const QString& line, QPointF pos);
    void submitOverlayLines(DebugDraw& dd, bool gpuFrame, QSize size);
    bool handleToolPress(QMouseEvent* e);
    void updateSculptBrush(QPointF pos);
    void tickTools(double dt);
    [[nodiscard]] Uuid entityUnder(QPointF pos);
    void maybeCreateOffscreenGpu();
    // input
    void onMousePress(QMouseEvent* e);
    void onMouseMove(QMouseEvent* e);
    void onMouseRelease(QMouseEvent* e);
    void onWheel(QWheelEvent* e);
    void onKey(QKeyEvent* e, bool down);

    EditorContext* m_ctx;
    ViewportCanvas* m_canvas = nullptr;            // software surface
    VulkanViewportWindow* m_vkWindow = nullptr;    // Vulkan surface (rhi), embedded via createWindowContainer
    QWidget* m_surface = nullptr;                  // whichever of the two is shown
    void setSurfaceCursor(Qt::CursorShape c);
    void unsetSurfaceCursor();
    void switchToSoftware(const QString& reason);
    QWidget* m_toolbar = nullptr;
    QToolButton* m_viewModeButton = nullptr;
    QToolButton* m_cameraButton = nullptr;
    QLabel* m_speedLabel = nullptr;
    std::unique_ptr<PainterViewportRenderer> m_painterRenderer; // software renderer (always available)
    std::unique_ptr<IViewportRenderer> m_gpuRenderer;           // render module, on the viewport's device
    std::shared_ptr<void> m_headlessDevice;                     // offscreen GPU canvas (screenshots, headless)
    ViewportCamera m_camera;
    TransformGizmo m_gizmo;
    ViewMode m_viewMode = ViewMode::Lit;
    bool m_gameExposure = true; // EV100 of the primary camera (else m_manualEv)
    float m_manualEv = 15.0f;
    [[nodiscard]] float viewEv100() const;
    ShowFlags m_flags;
    QTimer m_timer;
    QElapsedTimer m_clock;
    QElapsedTimer m_fpsClock;
    int m_fpsFrames = 0;
    double m_fps = 0.0;
    double m_frameMs = 0.0;
    bool m_dirty = true;
    // input state
    QSet<int> m_keys;
    bool m_flying = false;
    bool m_orbiting = false;
    bool m_panning = false;
    bool m_dollying = false;
    bool m_selecting = false;
    QPointF m_pressPos;
    QPointF m_lastPos;
    QRectF m_marquee;
    glm::vec3 m_orbitPivot{0.0f};
    float m_velocityBoost = 1.0f;
    // gizmo drag
    struct DragStart {
        Uuid id;
        Transform world;
        glm::mat4 parentWorld{1.0f};
    };
    std::vector<DragStart> m_dragStart;
    bool m_gizmoDragging = false;
    glm::vec3 m_splineDragStart{0.0f};
    // play input
    bool m_captured = false;
    QPointF m_captureCenter;
    bool m_ignoreNextMove = false;
    // terrain sculpt
    std::optional<glm::vec3> m_brushPos;
    Uuid m_brushTerrain;
    bool m_sculpting = false;
};

} // namespace ox::editor
