#pragma once

#include <oxwald/core/types.hpp>
#include <oxwald/spline/spline.hpp>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <functional>

// Debug visualisation without a renderer dependency: everything is emitted as line segments.
// Hook the callback up to the engine's debug-draw collector once it exists.
namespace ox::spline {

using LineCallback = std::function<void(glm::vec3 from, glm::vec3 to, glm::vec4 color)>;

struct DebugDrawOptions {
    u32 samplesPerSegment = 24;
    f32 pointSize = 0.1f;   // half extent of the control point crosses
    f32 frameSpacing = 1.0f; // distance between drawn frames, <= 0 disables frames
    f32 frameLength = 0.5f;
    bool drawPoints = true;
    bool drawHandles = true;
    glm::vec4 curveColor{1.0f, 1.0f, 1.0f, 1.0f};
    glm::vec4 pointColor{1.0f, 0.8f, 0.1f, 1.0f};
    glm::vec4 handleColor{0.3f, 0.7f, 1.0f, 1.0f};
};

void drawCurve(const Spline& spline, const LineCallback& line, const glm::vec4& color, u32 samplesPerSegment = 24);
void drawControlPoints(const Spline& spline, const LineCallback& line, const glm::vec4& color, f32 size = 0.1f);
// Bézier in/out handles (no-op for other types; for B-spline/NURBS draws the control polygon instead).
void drawHandles(const Spline& spline, const LineCallback& line, const glm::vec4& color);
// Tangent (blue), normal (green) and binormal (red) every `spacing` units.
void drawFrames(const Spline& spline, const LineCallback& line, f32 spacing, f32 axisLength);
void drawSpline(const Spline& spline, const LineCallback& line, const DebugDrawOptions& options = {});

} // namespace ox::spline
