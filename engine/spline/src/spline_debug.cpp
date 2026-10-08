#include <oxwald/spline/spline_debug.hpp>

#include <algorithm>

namespace ox::spline {

void drawCurve(const Spline& spline, const LineCallback& line, const glm::vec4& color, u32 samplesPerSegment) {
    const u32 segs = spline.segmentCount();
    if (segs == 0 || !line) {
        return;
    }
    const u32 steps = segs * std::max(samplesPerSegment, 1u);
    glm::vec3 prev = spline.position(0.0f);
    for (u32 i = 1; i <= steps; ++i) {
        const glm::vec3 p = spline.position(spline.maxT() * static_cast<f32>(i) / static_cast<f32>(steps));
        line(prev, p, color);
        prev = p;
    }
}

void drawControlPoints(const Spline& spline, const LineCallback& line, const glm::vec4& color, f32 size) {
    if (!line) {
        return;
    }
    for (const ControlPoint& cp : spline.points()) {
        const glm::vec3& p = cp.position;
        line(p - glm::vec3(size, 0, 0), p + glm::vec3(size, 0, 0), color);
        line(p - glm::vec3(0, size, 0), p + glm::vec3(0, size, 0), color);
        line(p - glm::vec3(0, 0, size), p + glm::vec3(0, 0, size), color);
    }
}

void drawHandles(const Spline& spline, const LineCallback& line, const glm::vec4& color) {
    if (!line) {
        return;
    }
    const auto& pts = spline.points();
    if (spline.type() == SplineType::Bezier) {
        for (const ControlPoint& cp : pts) {
            line(cp.position, cp.position + cp.inHandle, color);
            line(cp.position, cp.position + cp.outHandle, color);
        }
    } else if (spline.type() == SplineType::BSpline || spline.type() == SplineType::Nurbs) {
        for (usize i = 0; i + 1 < pts.size(); ++i) {
            line(pts[i].position, pts[i + 1].position, color);
        }
        if (spline.closed() && pts.size() >= 3) {
            line(pts.back().position, pts.front().position, color);
        }
    }
}

void drawFrames(const Spline& spline, const LineCallback& line, f32 spacing, f32 axisLength) {
    if (!line || spacing <= 0.0f || spline.segmentCount() == 0) {
        return;
    }
    for (const SplineSample& s : spline.sampleByDistance(spacing)) {
        line(s.position, s.position + s.tangent * axisLength, {0.2f, 0.4f, 1.0f, 1.0f});
        line(s.position, s.position + s.normal * axisLength, {0.2f, 1.0f, 0.2f, 1.0f});
        line(s.position, s.position + s.binormal * axisLength, {1.0f, 0.2f, 0.2f, 1.0f});
    }
}

void drawSpline(const Spline& spline, const LineCallback& line, const DebugDrawOptions& options) {
    drawCurve(spline, line, options.curveColor, options.samplesPerSegment);
    if (options.drawPoints) {
        drawControlPoints(spline, line, options.pointColor, options.pointSize);
    }
    if (options.drawHandles) {
        drawHandles(spline, line, options.handleColor);
    }
    if (options.frameSpacing > 0.0f) {
        drawFrames(spline, line, options.frameSpacing, options.frameLength);
    }
}

} // namespace ox::spline
