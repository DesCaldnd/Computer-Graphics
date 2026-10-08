#pragma once

#include "viewport/viewport_renderer.hpp"

#include <oxwald/core/debug_draw.hpp>
#include <oxwald/core/math.hpp>

#include <QPointF>
#include <QString>

class QPainter;

namespace ox::editor {

enum class GizmoMode { Select, Translate, Rotate, Scale };
enum class GizmoSpace { World, Local };

enum GizmoAxis : u8 {
    AxisNone = 0,
    AxisX = 1,
    AxisY = 2,
    AxisZ = 4,
    AxisXY = AxisX | AxisY,
    AxisXZ = AxisX | AxisZ,
    AxisYZ = AxisY | AxisZ,
    AxisAll = AxisX | AxisY | AxisZ, // screen-space translate / uniform scale / view-axis rotate
};

// Camera state the gizmo needs for projection and picking rays.
struct GizmoView {
    ViewportCamera camera;
    glm::mat4 view{1.0f};
    glm::mat4 proj{1.0f};
    glm::mat4 viewProj{1.0f};
    glm::vec2 size{1.0f};

    static GizmoView from(const ViewportCamera& cam, glm::vec2 sizePx);
    [[nodiscard]] Ray ray(QPointF pixel) const;
    // false when behind the camera
    bool project(glm::vec3 p, QPointF& out) const;
};

// CPU transform gizmo (translate/rotate/scale, world/local, snapping). Drawing goes through QPainter for the
// software viewport and through DebugDraw overlay lines for GPU renderers.
class TransformGizmo {
public:
    struct Snap {
        bool enabled = false;
        float translate = 0.25f;
        float rotateDeg = 15.0f;
        float scale = 0.1f;
    };
    struct Delta {
        glm::vec3 translation{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; // world-space rotation about the pivot
        glm::vec3 scale{1.0f};                       // multiplicative, in the gizmo's axes
        float angleDeg = 0.0f;
    };

    void setMode(GizmoMode m) { m_mode = m; }
    [[nodiscard]] GizmoMode mode() const { return m_mode; }
    void setSpace(GizmoSpace s) { m_space = s; }
    [[nodiscard]] GizmoSpace space() const { return m_space; }
    void setSnap(const Snap& s) { m_snap = s; }
    [[nodiscard]] const Snap& snap() const { return m_snap; }
    void setSizeScale(float s) { m_sizeScale = s; }
    void setTarget(glm::vec3 pivot, glm::quat orientation);
    void setVisible(bool v) { m_visible = v; }
    [[nodiscard]] bool visible() const { return m_visible && m_mode != GizmoMode::Select; }

    [[nodiscard]] GizmoAxis hitTest(const GizmoView& view, QPointF mouse) const;
    void setHover(GizmoAxis a) { m_hover = a; }
    [[nodiscard]] GizmoAxis hover() const { return m_hover; }

    bool begin(const GizmoView& view, QPointF mouse, GizmoAxis axis);
    [[nodiscard]] Delta update(const GizmoView& view, QPointF mouse, bool snapOverride = false);
    void end();
    [[nodiscard]] bool active() const { return m_activeAxis != AxisNone; }
    [[nodiscard]] GizmoAxis activeAxis() const { return m_activeAxis; }
    [[nodiscard]] QString dragLabel() const { return m_label; }

    void draw(QPainter& p, const GizmoView& view) const;
    void emitLines(DebugDraw& dd, const GizmoView& view) const;

    // World length of the gizmo arms at the pivot (constant screen size).
    [[nodiscard]] float worldLength(const GizmoView& view) const;
    [[nodiscard]] glm::vec3 axisDir(int i) const;

private:
    [[nodiscard]] glm::vec3 dragPoint(const GizmoView& view, QPointF mouse, bool& ok) const;

    GizmoMode m_mode = GizmoMode::Translate;
    GizmoSpace m_space = GizmoSpace::World;
    Snap m_snap;
    float m_sizeScale = 1.0f;
    bool m_visible = false;
    glm::vec3 m_pivot{0.0f};
    glm::quat m_orient{1.0f, 0.0f, 0.0f, 0.0f};
    GizmoAxis m_hover = AxisNone;
    // drag state
    GizmoAxis m_activeAxis = AxisNone;
    glm::vec3 m_startPivot{0.0f};
    glm::vec3 m_startPoint{0.0f};
    glm::vec3 m_planeNormal{0.0f, 1.0f, 0.0f};
    float m_startAxisT = 0.0f;
    QPointF m_startMouse;
    float m_startLength = 1.0f;
    QString m_label;
};

} // namespace ox::editor
