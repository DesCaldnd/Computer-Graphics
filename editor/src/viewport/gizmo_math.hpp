#pragma once

// Qt-free math of the transform gizmo: the gizmo frame (World/Local), screen-space picking of rotated handles, drag
// projection onto (local) axes and planes, snapping in the gizmo frame and applying a drag to entity transforms
// (parent hierarchies with non-uniform scale included). TransformGizmo (gizmo.hpp) draws and drives it; the tests
// call it directly.

#include <oxwald/core/math.hpp>

#include <array>

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

namespace gizmo {

// Single axis handle → 0..2, anything else → -1.
[[nodiscard]] int axisIndex(GizmoAxis a);
// Plane handle → index of its normal axis (YZ → 0, XZ → 1, XY → 2), anything else → -1.
[[nodiscard]] int planeNormalIndex(GizmoAxis a);

// Orthonormal frame of a world matrix: X along column 0, Y = column 1 made orthogonal to X, Z = X × Y. Equals
// Transform::fromMatrix(m).rotation for matrices without shear (mirroring is folded into X like ox::decompose); under
// a non-uniformly scaled, rotated parent (sheared world matrix) the frame keeps the entity's visible X axis and XY
// plane. Degenerate (zero-scale) axes fall back to the remaining ones / identity.
[[nodiscard]] glm::quat frameRotation(const glm::mat3& linear);
[[nodiscard]] glm::quat frameRotation(const glm::mat4& world);

// Orientation the gizmo is drawn and dragged in: the entity frame in Local space, identity in World space. Scale is
// always local (as in Unreal/Unity): per-axis factors only make sense along the entity's own axes.
[[nodiscard]] glm::quat effectiveOrientation(GizmoMode mode, GizmoSpace space, glm::quat local);

[[nodiscard]] glm::vec3 axisOf(glm::quat frame, int i);

// Pixel-space projection (origin top-left, y down) of a reversed-Z view-projection matrix.
struct Projection {
    glm::mat4 viewProj{1.0f};
    glm::vec2 size{1.0f};
    // false when behind the camera
    bool project(glm::vec3 p, glm::vec2& out) const;
    [[nodiscard]] Ray ray(glm::vec2 pixel) const;
};

// The gizmo in the world: pivot, orientation (see effectiveOrientation) and arm length in world units.
struct Frame {
    glm::vec3 pivot{0.0f};
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
    float length = 1.0f;
    [[nodiscard]] glm::vec3 axis(int i) const { return axisOf(orientation, i); }
};

// Corners of the translate plane handle whose normal is axis `normal` (the square between 0.22 and 0.42 arm lengths
// along the two other, possibly rotated, axes).
[[nodiscard]] std::array<glm::vec3, 4> planeHandle(const Frame& f, int normal);
inline constexpr float kAxisPickPx = 9.0f;
inline constexpr float kRingPickPx = 8.0f;

// Screen-space picking of the (rotated) handles. `cameraRight` sizes the view ring of the rotate gizmo.
[[nodiscard]] GizmoAxis pickTranslate(const Projection& p, const Frame& f, glm::vec2 mouse);
[[nodiscard]] GizmoAxis pickScale(const Projection& p, const Frame& f, glm::vec2 mouse);
[[nodiscard]] GizmoAxis pickRotate(const Projection& p, const Frame& f, glm::vec2 mouse, glm::vec3 cameraRight);
[[nodiscard]] GizmoAxis pick(GizmoMode mode, const Projection& p, const Frame& f, glm::vec2 mouse, glm::vec3 cameraRight);

[[nodiscard]] float distToSegment(glm::vec2 p, glm::vec2 a, glm::vec2 b);
[[nodiscard]] bool pointInQuad(glm::vec2 p, const std::array<glm::vec2, 4>& quad);
// Parameter t along the line (o + t*d, d unit) closest to the ray; false when they are parallel.
bool closestOnLine(const Ray& ray, glm::vec3 o, glm::vec3 d, float& t);
// Ray hit with the plane through p0 with normal n (in front of the ray origin only).
bool rayPlane(const Ray& ray, glm::vec3 p0, glm::vec3 n, glm::vec3& out);
[[nodiscard]] float snapTo(float v, float step);

// Drag along one axis (unit world direction, e.g. a local axis): moved distance t - startT, snapped to `snapStep`
// along that axis (0 = no snapping).
[[nodiscard]] glm::vec3 axisTranslation(glm::vec3 dir, float startT, float t, float snapStep);
// Plane / screen drag: world delta between the plane hits, each component snapped in the gizmo frame.
[[nodiscard]] glm::vec3 planeTranslation(glm::vec3 startPoint, glm::vec3 point, glm::quat frame, float snapStep);
// Signed angle (radians) from v0 to v1 around the unit axis n.
[[nodiscard]] float signedAngle(glm::vec3 v0, glm::vec3 v1, glm::vec3 n);

// One dragged entity at drag start.
struct EditTarget {
    Transform local;               // TransformComponent values
    glm::mat4 parentWorld{1.0f};   // parent world matrix (identity for roots)
    [[nodiscard]] glm::vec3 worldPosition() const { return glm::vec3(parentWorld * glm::vec4(local.position, 1.0f)); }
    [[nodiscard]] glm::mat4 worldMatrix() const { return parentWorld * local.toMatrix(); }
};

// New local transforms for a world-space gizmo delta. Untouched components are kept bit-exact (no decomposition of
// sheared matrices), positions go through the inverse parent matrix.
[[nodiscard]] Transform applyTranslation(const EditTarget& t, glm::vec3 worldDelta);
// Rotation `worldRotation` about `pivot`. The entity's visible X axis turns exactly by the rotation even under a
// non-uniformly scaled parent (Y/Z follow orthonormalised).
[[nodiscard]] Transform applyRotation(const EditTarget& t, glm::vec3 pivot, glm::quat worldRotation);
// Scale factors along the entity's own local axes. With `aroundPivot` (multi-selection) the position spreads from the
// pivot along the gizmo frame axes too.
[[nodiscard]] Transform applyScale(const EditTarget& t, glm::vec3 pivot, glm::quat frame, glm::vec3 factors, bool aroundPivot);

} // namespace gizmo
} // namespace ox::editor
