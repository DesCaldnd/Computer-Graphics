#pragma once

#include <oxwald/core/types.hpp>
#include <oxwald/spline/bezier.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ox::spline {

enum class SplineType : u8 {
    Linear,     // polyline through the points
    Bezier,     // cubic Bézier through the points, shaped by in/out handles
    CatmullRom, // interpolating, alpha = SplineSettings::catmullRomAlpha
    BSpline,    // uniform cubic B-spline (clamped when open, periodic when closed); approximating
    Nurbs,      // degree p, per-point weights, clamped (or custom) knots; approximating
};

enum class HandleMode : u8 {
    Free,     // handles independent
    Aligned,  // collinear, independent lengths (G1)
    Mirrored, // collinear and equal length (C1)
    Auto,     // computed from neighbours (Catmull-Rom-like, C1); editing a handle switches the point to Aligned
};

enum class FrameMode : u8 {
    RotationMinimizing, // parallel transport (double reflection), seeded with SplineSettings::upVector
    UpVector,           // normal = per-point up (interpolated) or SplineSettings::upVector, made ⊥ to the tangent
};

inline constexpr f32 kCatmullRomUniform = 0.0f;
inline constexpr f32 kCatmullRomCentripetal = 0.5f;
inline constexpr f32 kCatmullRomChordal = 1.0f;
inline constexpr u32 kMaxNurbsDegree = 7;

struct ControlPoint {
    glm::vec3 position{0.0f};
    // Bézier handles as offsets relative to position (they move with the point).
    glm::vec3 inHandle{0.0f};
    glm::vec3 outHandle{0.0f};
    HandleMode handleMode = HandleMode::Auto;
    f32 roll = 0.0f;   // radians around the tangent (right-handed), interpolated linearly between points
    f32 weight = 1.0f; // NURBS only, must be > 0
    std::optional<glm::vec3> up; // FrameMode::UpVector only; unset → SplineSettings::upVector
};

struct SplineSettings {
    f32 catmullRomAlpha = kCatmullRomCentripetal;
    u32 degree = 3;           // Nurbs only, clamped to [1, min(kMaxNurbsDegree, pointCount-1)]
    std::vector<f32> knots;   // Nurbs, open only: custom knot vector of size pointCount+degree+1 (else clamped uniform)
    FrameMode frameMode = FrameMode::RotationMinimizing;
    glm::vec3 upVector{0.0f, 1.0f, 0.0f};
};

// Result of Spline::evaluate. Frame convention (matches the engine camera convention):
// tangent = forward along increasing t, normal = "up", binormal = cross(tangent, normal) = "right".
struct SplineSample {
    f32 t = 0.0f;
    glm::vec3 position{0.0f};
    glm::vec3 tangent{0.0f, 0.0f, -1.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec3 binormal{1.0f, 0.0f, 0.0f};
    glm::vec3 derivative{0.0f};       // dP/dt (not normalised)
    glm::vec3 secondDerivative{0.0f}; // d²P/dt²
    f32 curvature = 0.0f;             // 1/radius
    f32 roll = 0.0f;                  // interpolated roll already applied to normal/binormal

    // Rotation mapping localForward → tangent and localUp → normal (defaults: −Z forward, +Y up).
    glm::quat rotation(const glm::vec3& localForward = {0.0f, 0.0f, -1.0f},
                       const glm::vec3& localUp = {0.0f, 1.0f, 0.0f}) const;
};

struct ClosestPointResult {
    f32 t = 0.0f;
    glm::vec3 position{0.0f};
    f32 distance = 0.0f; // world distance from the query point
};

struct Bounds {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
};

// Named parameter on the spline (e.g. "station", "checkpoint_3"); PathFollower fires them when crossed.
struct SplineMarker {
    std::string name;
    f32 t = 0.0f;
};

// Builds the rotation that maps (localForward, localUp) onto (forward, up). forward/up need not be orthogonal.
glm::quat frameRotation(const glm::vec3& forward, const glm::vec3& up,
                        const glm::vec3& localForward = {0.0f, 0.0f, -1.0f},
                        const glm::vec3& localUp = {0.0f, 1.0f, 0.0f});

namespace detail {
inline constexpr u32 kSubSteps = 16; // per-segment arc-length LUT steps and RMF frame samples

struct Segment {
    CubicBezier bezier{};       // cubic kinds
    f32 u0 = 0.0f, u1 = 0.0f;   // NURBS knot interval
    u32 span = 0;               // NURBS knot span index
    glm::vec3 hullMin{0.0f}, hullMax{0.0f}; // conservative (convex hull) box
    f32 startDistance = 0.0f;
    f32 length = 0.0f;
    std::array<f32, kSubSteps + 1> lut{}; // cumulative length at local u = k / kSubSteps
};

struct FrameSample {
    glm::vec3 position{0.0f};
    glm::vec3 tangent{0.0f};
    glm::vec3 normal{0.0f};
    f32 correction = 0.0f; // closed-loop holonomy correction angle
};

struct Cache {
    bool dirty = true;
    bool cubic = true;
    std::vector<Segment> segments;
    std::vector<glm::vec4> homogeneous; // NURBS control points (w·P, w), periodic ones already wrapped
    std::vector<f32> knots;
    u32 degree = 3;
    f32 length = 0.0f;
    std::vector<FrameSample> frames;
};
} // namespace detail

// A 3D curve with a lazily rebuilt evaluation cache.
//
// Parametrisation: t ∈ [0, segmentCount()], segment i covers [i, i+1]. For Bézier/Catmull-Rom/Linear the
// control point i sits at t = i. For B-spline/NURBS segments are non-empty knot spans (control points are
// generally not on the curve). Closed splines wrap t modulo segmentCount(); open ones clamp.
// Distances are arc lengths in the spline's local space ∈ [0, length()].
//
// Const queries rebuild the cache on demand: call rebuild() before sharing a spline across threads.
class Spline {
public:
    Spline() = default;
    explicit Spline(SplineType type, bool closed = false);

    SplineType type() const { return m_type; }
    void setType(SplineType type);
    bool closed() const { return m_closed; }
    void setClosed(bool closed);
    const SplineSettings& settings() const { return m_settings; }
    void setSettings(SplineSettings settings);

    // Incremented on every modification; cheap change detection for caches built on top (meshes, ECS).
    u64 version() const { return m_version; }

    // --- Control points -------------------------------------------------------------------------
    usize pointCount() const { return m_points.size(); }
    const std::vector<ControlPoint>& points() const { return m_points; }
    const ControlPoint& point(usize index) const;
    void setPoints(std::vector<ControlPoint> points);
    void addPoint(const glm::vec3& position);
    void addPoint(const ControlPoint& point);
    void insertPoint(usize index, const ControlPoint& point);
    void removePoint(usize index);
    void clear();
    void setPoint(usize index, const ControlPoint& point);
    void setPosition(usize index, const glm::vec3& position);
    // Setting one handle enforces the point's handle mode on the other one.
    void setInHandle(usize index, const glm::vec3& offset);
    void setOutHandle(usize index, const glm::vec3& offset);
    void setHandleMode(usize index, HandleMode mode);
    void setRoll(usize index, f32 roll);
    void setWeight(usize index, f32 weight);
    void setUp(usize index, std::optional<glm::vec3> up);
    // Recomputes the handles of every HandleMode::Auto point (done automatically by the setters).
    void recomputeAutoHandles();
    // Inserts a point at parameter t. For Bézier the segment is split with de Casteljau and the shape is
    // preserved exactly (affected neighbours in Mirrored/Auto mode become Aligned). Other types insert
    // the curve point at t (interpolating types keep the shape approximately). Returns the new index.
    usize insertPointAt(f32 t);

    // --- Markers --------------------------------------------------------------------------------
    void addMarker(std::string name, f32 t);
    bool removeMarker(std::string_view name);
    const std::vector<SplineMarker>& markers() const { return m_markers; }
    std::optional<f32> markerT(std::string_view name) const;

    // --- Evaluation -----------------------------------------------------------------------------
    u32 segmentCount() const;
    f32 maxT() const { return static_cast<f32>(segmentCount()); }
    glm::vec3 position(f32 t) const;
    glm::vec3 derivative(f32 t) const;
    glm::vec3 secondDerivative(f32 t) const;
    glm::vec3 tangent(f32 t) const;
    f32 curvature(f32 t) const;
    SplineSample evaluate(f32 t) const;
    SplineSample evaluateAtDistance(f32 distance) const { return evaluate(distanceToT(distance)); }

    // --- Arc length -----------------------------------------------------------------------------
    f32 length() const;
    f32 segmentLength(u32 segment) const;
    f32 tToDistance(f32 t) const;
    f32 distanceToT(f32 distance) const;
    // count+1 parameters at equal arc-length intervals, both ends included (closed: last == maxT()).
    std::vector<f32> uniformParameters(u32 intervals) const;
    // Samples every ~spacing units; spacing is adjusted so the last sample lands exactly on the end.
    std::vector<SplineSample> sampleByDistance(f32 spacing) const;

    ClosestPointResult closestPoint(const glm::vec3& worldPoint) const;
    Bounds bounds() const;

    void rebuild() const;

private:
    void touch();
    void ensureCache() const;
    void buildCubicSegments() const;
    void buildNurbsSegments() const;
    void buildArcLength() const;
    void buildFrames() const;

    struct Location {
        u32 segment = 0;
        f32 u = 0.0f;
    };
    Location locate(f32 t) const;
    void evalSegment(u32 segment, f32 u, glm::vec3* p, glm::vec3* d1, glm::vec3* d2) const;
    glm::vec3 safeTangent(u32 segment, f32 u) const;
    f32 segmentDistance(u32 segment, f32 u) const;
    f32 distanceToTClamped(f32 distance) const;
    f32 rollAt(f32 t) const;
    glm::vec3 upAt(f32 t) const;
    void enforceHandleMode(usize index, bool outMoved);

    SplineType m_type = SplineType::Bezier;
    bool m_closed = false;
    SplineSettings m_settings;
    std::vector<ControlPoint> m_points;
    std::vector<SplineMarker> m_markers;
    u64 m_version = 0;
    mutable detail::Cache m_cache;
};

} // namespace ox::spline
