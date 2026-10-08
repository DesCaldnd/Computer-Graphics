#include "viewport/gizmo.hpp"

#include "theme/theme.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>

#include <cmath>

namespace ox::editor {

GizmoView GizmoView::from(const ViewportCamera& cam, glm::vec2 sizePx) {
    GizmoView v;
    v.camera = cam;
    v.size = glm::max(sizePx, glm::vec2(1.0f));
    v.view = cam.view();
    v.proj = cam.projection(v.size.x / v.size.y);
    v.viewProj = v.proj * v.view;
    return v;
}

Ray GizmoView::ray(QPointF pixel) const {
    const glm::vec2 ndc{float(pixel.x()) / size.x * 2.0f - 1.0f, 1.0f - float(pixel.y()) / size.y * 2.0f};
    const glm::mat4 inv = glm::inverse(viewProj);
    // reversed Z: depth 1 = near, ~0 = far
    glm::vec4 n = inv * glm::vec4(ndc, 1.0f, 1.0f);
    glm::vec4 f = inv * glm::vec4(ndc, 0.01f, 1.0f);
    n /= n.w;
    f /= f.w;
    Ray r;
    r.origin = glm::vec3(n);
    r.direction = glm::normalize(glm::vec3(f) - glm::vec3(n));
    return r;
}

bool GizmoView::project(glm::vec3 p, QPointF& out) const {
    const glm::vec4 c = viewProj * glm::vec4(p, 1.0f);
    if (c.w <= 1e-4f) return false;
    const glm::vec3 ndc = glm::vec3(c) / c.w;
    out = QPointF((ndc.x * 0.5 + 0.5) * size.x, (1.0 - (ndc.y * 0.5 + 0.5)) * size.y);
    return true;
}

namespace {

float distToSegment(QPointF p, QPointF a, QPointF b) {
    const QPointF ab = b - a;
    const double len2 = ab.x() * ab.x() + ab.y() * ab.y();
    double t = len2 > 1e-9 ? ((p - a).x() * ab.x() + (p - a).y() * ab.y()) / len2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    const QPointF q = a + ab * t;
    return float(std::hypot(p.x() - q.x(), p.y() - q.y()));
}

// Parameter t along the line (o + t*d) closest to the ray.
bool closestOnLine(const Ray& ray, glm::vec3 o, glm::vec3 d, float& t) {
    const glm::vec3 w0 = o - ray.origin;
    const float b = glm::dot(ray.direction, d);
    const float denom = 1.0f - b * b;
    if (std::abs(denom) < 1e-5f) return false;
    const float dd = glm::dot(ray.direction, w0);
    const float e = glm::dot(d, w0);
    t = (b * dd - e) / denom;
    return true;
}

bool rayPlane(const Ray& ray, glm::vec3 p0, glm::vec3 n, glm::vec3& out) {
    const float denom = glm::dot(n, ray.direction);
    if (std::abs(denom) < 1e-5f) return false;
    const float t = glm::dot(p0 - ray.origin, n) / denom;
    if (t < 0.0f) return false;
    out = ray.origin + ray.direction * t;
    return true;
}

float snapTo(float v, float step) { return step > 0.0f ? std::round(v / step) * step : v; }

QColor axisColor(int i, bool hot, bool dim) {
    QColor c = Theme::axisColor(i);
    if (hot) c = QColor(255, 214, 64);
    if (dim) c.setAlpha(70);
    return c;
}

} // namespace

void TransformGizmo::setTarget(glm::vec3 pivot, glm::quat orientation) {
    m_pivot = pivot;
    m_orient = m_space == GizmoSpace::Local ? glm::normalize(orientation) : glm::quat(1, 0, 0, 0);
}

glm::vec3 TransformGizmo::axisDir(int i) const {
    glm::vec3 e(0.0f);
    e[i] = 1.0f;
    return glm::normalize(m_orient * e);
}

float TransformGizmo::worldLength(const GizmoView& view) const {
    if (view.camera.orthographic) return view.camera.orthoHeight * 0.14f * m_sizeScale;
    const float dist = glm::length(m_pivot - view.camera.position);
    const float fovK = std::tan(toRadians(view.camera.verticalFovDeg) * 0.5f) / std::tan(toRadians(30.0f));
    return std::max(0.001f, dist * 0.16f * fovK * m_sizeScale * (720.0f / std::max(300.0f, view.size.y)));
}

GizmoAxis TransformGizmo::hitTest(const GizmoView& view, QPointF mouse) const {
    if (!visible()) return AxisNone;
    const float len = worldLength(view);
    QPointF c;
    if (!view.project(m_pivot, c)) return AxisNone;
    if (m_mode == GizmoMode::Translate) {
        // plane handles
        const GizmoAxis planes[3] = {AxisYZ, AxisXZ, AxisXY};
        for (int i = 0; i < 3; ++i) {
            const glm::vec3 a = axisDir((i + 1) % 3), b = axisDir((i + 2) % 3);
            QPolygonF quad;
            for (glm::vec2 k : {glm::vec2(0.22f, 0.22f), glm::vec2(0.42f, 0.22f), glm::vec2(0.42f, 0.42f), glm::vec2(0.22f, 0.42f)}) {
                QPointF q;
                if (!view.project(m_pivot + (a * k.x + b * k.y) * len, q)) return AxisNone;
                quad << q;
            }
            if (quad.containsPoint(mouse, Qt::OddEvenFill)) return planes[i];
        }
        if (std::hypot(mouse.x() - c.x(), mouse.y() - c.y()) < 7.0) return AxisAll;
    }
    if (m_mode == GizmoMode::Translate || m_mode == GizmoMode::Scale) {
        if (m_mode == GizmoMode::Scale && std::hypot(mouse.x() - c.x(), mouse.y() - c.y()) < 9.0) return AxisAll;
        float best = 9.0f;
        GizmoAxis hit = AxisNone;
        for (int i = 0; i < 3; ++i) {
            QPointF tip;
            if (!view.project(m_pivot + axisDir(i) * len * 1.12f, tip)) continue;
            const float d = distToSegment(mouse, c, tip);
            if (d < best) {
                best = d;
                hit = GizmoAxis(1 << i);
            }
        }
        return hit;
    }
    if (m_mode == GizmoMode::Rotate) {
        float best = 8.0f;
        GizmoAxis hit = AxisNone;
        for (int i = 0; i < 3; ++i) {
            const glm::vec3 u = axisDir((i + 1) % 3), v = axisDir((i + 2) % 3);
            QPointF prev;
            bool havePrev = false;
            for (int s = 0; s <= 64; ++s) {
                const float a = float(s) / 64.0f * kTwoPi;
                QPointF q;
                const bool ok = view.project(m_pivot + (u * std::cos(a) + v * std::sin(a)) * len, q);
                if (ok && havePrev) {
                    const float d = distToSegment(mouse, prev, q);
                    if (d < best) {
                        best = d;
                        hit = GizmoAxis(1 << i);
                    }
                }
                prev = q;
                havePrev = ok;
            }
        }
        if (hit == AxisNone) {
            // outer view ring
            const double r = std::hypot(mouse.x() - c.x(), mouse.y() - c.y());
            QPointF edge;
            view.project(m_pivot + view.camera.right() * len * 1.2f, edge);
            const double ringR = std::hypot(edge.x() - c.x(), edge.y() - c.y());
            if (std::abs(r - ringR) < 7.0) hit = AxisAll;
        }
        return hit;
    }
    return AxisNone;
}

glm::vec3 TransformGizmo::dragPoint(const GizmoView& view, QPointF mouse, bool& ok) const {
    const Ray r = view.ray(mouse);
    glm::vec3 p(0.0f);
    ok = rayPlane(r, m_startPivot, m_planeNormal, p);
    return p;
}

bool TransformGizmo::begin(const GizmoView& view, QPointF mouse, GizmoAxis axis) {
    if (axis == AxisNone) return false;
    m_activeAxis = axis;
    m_startPivot = m_pivot;
    m_startMouse = mouse;
    m_startLength = worldLength(view);
    m_label.clear();
    const glm::vec3 toCam = view.camera.orthographic ? -view.camera.forward() : glm::normalize(view.camera.position - m_pivot);
    if (m_mode == GizmoMode::Translate || m_mode == GizmoMode::Scale) {
        if (axis == AxisX || axis == AxisY || axis == AxisZ) {
            const int i = axis == AxisX ? 0 : axis == AxisY ? 1 : 2;
            float t = 0.0f;
            closestOnLine(view.ray(mouse), m_pivot, axisDir(i), t);
            m_startAxisT = t;
        } else if (axis == AxisAll) {
            m_planeNormal = toCam;
        } else {
            const int n = axis == AxisYZ ? 0 : axis == AxisXZ ? 1 : 2;
            m_planeNormal = axisDir(n);
        }
    } else if (m_mode == GizmoMode::Rotate) {
        m_planeNormal = axis == AxisAll ? toCam : axisDir(axis == AxisX ? 0 : axis == AxisY ? 1 : 2);
    }
    bool ok = false;
    m_startPoint = dragPoint(view, mouse, ok);
    return true;
}

TransformGizmo::Delta TransformGizmo::update(const GizmoView& view, QPointF mouse, bool snapOverride) {
    Delta d;
    if (!active()) return d;
    const bool snap = m_snap.enabled != snapOverride;
    const Ray r = view.ray(mouse);
    const GizmoAxis a = m_activeAxis;
    const bool single = a == AxisX || a == AxisY || a == AxisZ;
    const int ai = a == AxisX ? 0 : a == AxisY ? 1 : 2;
    if (m_mode == GizmoMode::Translate) {
        glm::vec3 delta(0.0f);
        if (single) {
            float t = m_startAxisT;
            if (closestOnLine(r, m_startPivot, axisDir(ai), t)) {
                float dt = t - m_startAxisT;
                if (snap) dt = snapTo(dt, m_snap.translate);
                delta = axisDir(ai) * dt;
            }
        } else {
            bool ok = false;
            const glm::vec3 p = dragPoint(view, mouse, ok);
            if (ok) delta = p - m_startPoint;
            if (snap) {
                // snap in the gizmo's frame
                glm::vec3 local = glm::inverse(m_orient) * delta;
                for (int i = 0; i < 3; ++i) local[i] = snapTo(local[i], m_snap.translate);
                delta = m_orient * local;
            }
        }
        d.translation = delta;
        m_pivot = m_startPivot + delta;
        m_label = QStringLiteral("Δ %1, %2, %3").arg(delta.x, 0, 'f', 2).arg(delta.y, 0, 'f', 2).arg(delta.z, 0, 'f', 2);
    } else if (m_mode == GizmoMode::Rotate) {
        float angle = 0.0f;
        bool ok = false;
        const glm::vec3 p = dragPoint(view, mouse, ok);
        const float facing = std::abs(glm::dot(r.direction, m_planeNormal));
        if (ok && facing > 0.12f) {
            const glm::vec3 v0 = m_startPoint - m_startPivot, v1 = p - m_startPivot;
            angle = std::atan2(glm::dot(m_planeNormal, glm::cross(v0, v1)), glm::dot(v0, v1));
        } else {
            angle = float(mouse.x() - m_startMouse.x()) * 0.01f;
        }
        float deg = glm::degrees(angle);
        if (snap) deg = snapTo(deg, m_snap.rotateDeg);
        d.angleDeg = deg;
        d.rotation = glm::angleAxis(glm::radians(deg), m_planeNormal);
        m_label = QStringLiteral("%1°").arg(deg, 0, 'f', 1);
    } else if (m_mode == GizmoMode::Scale) {
        glm::vec3 s(1.0f);
        if (single) {
            float t = m_startAxisT;
            if (closestOnLine(r, m_startPivot, axisDir(ai), t) && std::abs(m_startAxisT) > 1e-4f) {
                float f = std::max(0.01f, 1.0f + (t - m_startAxisT) / m_startLength);
                if (snap) f = std::max(m_snap.scale, snapTo(f, m_snap.scale));
                s[ai] = f;
            }
        } else {
            float f = std::pow(2.0f, float(m_startMouse.y() - mouse.y()) / 120.0f);
            if (snap) f = std::max(m_snap.scale, snapTo(f, m_snap.scale));
            if (a == AxisAll) s = glm::vec3(f);
            else {
                if (a & AxisX) s.x = f;
                if (a & AxisY) s.y = f;
                if (a & AxisZ) s.z = f;
            }
        }
        d.scale = s;
        m_label = QStringLiteral("× %1, %2, %3").arg(s.x, 0, 'f', 2).arg(s.y, 0, 'f', 2).arg(s.z, 0, 'f', 2);
    }
    return d;
}

void TransformGizmo::end() {
    m_activeAxis = AxisNone;
    m_label.clear();
}

void TransformGizmo::draw(QPainter& p, const GizmoView& view) const {
    if (!visible()) return;
    QPointF c;
    if (!view.project(m_pivot, c)) return;
    const float len = worldLength(view);
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    const GizmoAxis hot = active() ? m_activeAxis : m_hover;
    auto isHot = [&](GizmoAxis a) { return hot == a; };
    auto dimmed = [&](GizmoAxis a) { return active() && m_activeAxis != a && !(m_activeAxis & a && m_activeAxis != AxisAll && (a == AxisX || a == AxisY || a == AxisZ)); };

    if (m_mode == GizmoMode::Rotate) {
        for (int i = 0; i < 3; ++i) {
            const GizmoAxis a = GizmoAxis(1 << i);
            const glm::vec3 u = axisDir((i + 1) % 3), v = axisDir((i + 2) % 3);
            const glm::vec3 toCam = view.camera.orthographic ? -view.camera.forward() : glm::normalize(view.camera.position - m_pivot);
            QColor col = axisColor(i, isHot(a), dimmed(a));
            QPointF prev;
            bool havePrev = false;
            for (int s = 0; s <= 72; ++s) {
                const float ang = float(s) / 72.0f * kTwoPi;
                const glm::vec3 dir = u * std::cos(ang) + v * std::sin(ang);
                QPointF q;
                const bool ok = view.project(m_pivot + dir * len, q);
                if (ok && havePrev) {
                    const bool front = glm::dot(dir, toCam) > -0.05f;
                    QColor cc = col;
                    if (!front) cc.setAlpha(cc.alpha() / 3);
                    p.setPen(QPen(cc, isHot(a) ? 3.2 : 2.2, Qt::SolidLine, Qt::RoundCap));
                    p.drawLine(prev, q);
                }
                prev = q;
                havePrev = ok;
            }
        }
        QPointF edge;
        view.project(m_pivot + view.camera.right() * len * 1.2f, edge);
        const double ringR = std::hypot(edge.x() - c.x(), edge.y() - c.y());
        p.setPen(QPen(isHot(AxisAll) ? QColor(255, 214, 64) : QColor(255, 255, 255, active() && !isHot(AxisAll) ? 50 : 150), isHot(AxisAll) ? 2.6 : 1.6));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(c, ringR, ringR);
        p.setBrush(QColor(255, 255, 255, 18));
        p.setPen(Qt::NoPen);
        p.drawEllipse(c, ringR * 0.83, ringR * 0.83);
    } else {
        // plane handles (translate)
        if (m_mode == GizmoMode::Translate) {
            const GizmoAxis planes[3] = {AxisYZ, AxisXZ, AxisXY};
            for (int i = 0; i < 3; ++i) {
                const glm::vec3 a = axisDir((i + 1) % 3), b = axisDir((i + 2) % 3);
                QPolygonF quad;
                bool ok = true;
                for (glm::vec2 k : {glm::vec2(0.22f, 0.22f), glm::vec2(0.42f, 0.22f), glm::vec2(0.42f, 0.42f), glm::vec2(0.22f, 0.42f)}) {
                    QPointF q;
                    ok &= view.project(m_pivot + (a * k.x + b * k.y) * len, q);
                    quad << q;
                }
                if (!ok) continue;
                QColor col = axisColor(i, isHot(planes[i]), dimmed(planes[i]));
                QColor fill = col;
                fill.setAlpha(isHot(planes[i]) ? 140 : dimmed(planes[i]) ? 25 : 70);
                p.setBrush(fill);
                p.setPen(QPen(col, 1.2));
                p.drawPolygon(quad);
            }
        }
        // axes, back to front
        int order[3] = {0, 1, 2};
        std::sort(order, order + 3, [&](int x, int y) {
            return glm::dot(axisDir(x), view.camera.forward()) > glm::dot(axisDir(y), view.camera.forward());
        });
        for (int i : order) {
            const GizmoAxis a = GizmoAxis(1 << i);
            QPointF tip, base;
            if (!view.project(m_pivot + axisDir(i) * len, tip) || !view.project(m_pivot + axisDir(i) * len * 0.78f, base)) continue;
            QColor col = axisColor(i, isHot(a), dimmed(a));
            p.setPen(QPen(col, isHot(a) ? 3.4 : 2.4, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(c, m_mode == GizmoMode::Translate ? base : tip);
            if (m_mode == GizmoMode::Translate) {
                const QPointF dir = tip - base;
                const double l = std::hypot(dir.x(), dir.y());
                QPointF n = l > 1e-3 ? QPointF(-dir.y() / l, dir.x() / l) : QPointF(0, 0);
                const double w = std::max(4.5, l * 0.45);
                QPolygonF tri;
                tri << tip << base + n * w << base - n * w;
                p.setBrush(col);
                p.setPen(Qt::NoPen);
                p.drawPolygon(tri);
            } else {
                p.setBrush(col);
                p.setPen(Qt::NoPen);
                p.drawRoundedRect(QRectF(tip.x() - 5, tip.y() - 5, 10, 10), 2, 2);
            }
        }
        // center
        const bool centerHot = isHot(AxisAll);
        p.setPen(QPen(centerHot ? QColor(255, 214, 64) : QColor(255, 255, 255, 220), 1.5));
        p.setBrush(centerHot ? QColor(255, 214, 64, 160) : QColor(255, 255, 255, 60));
        if (m_mode == GizmoMode::Scale) p.drawRoundedRect(QRectF(c.x() - 6, c.y() - 6, 12, 12), 2, 2);
        else p.drawEllipse(c, 5.0, 5.0);
    }
    p.restore();
}

void TransformGizmo::emitLines(DebugDraw& dd, const GizmoView& view) const {
    if (!visible()) return;
    const float len = worldLength(view);
    for (int i = 0; i < 3; ++i) {
        const GizmoAxis a = GizmoAxis(1 << i);
        const QColor q = axisColor(i, (active() ? m_activeAxis : m_hover) == a, false);
        const glm::vec4 col(q.redF(), q.greenF(), q.blueF(), q.alphaF());
        if (m_mode == GizmoMode::Rotate) {
            dd.circle(m_pivot, axisDir(i), len, col, 0.0f, false, 64);
        } else if (m_mode == GizmoMode::Translate) {
            dd.arrow(m_pivot, m_pivot + axisDir(i) * len, len * 0.2f, col, 0.0f, false);
        } else {
            dd.line(m_pivot, m_pivot + axisDir(i) * len, col, 0.0f, false);
            dd.box(m_pivot + axisDir(i) * len, glm::vec3(len * 0.05f), m_orient, col, 0.0f, false);
        }
    }
}

} // namespace ox::editor
