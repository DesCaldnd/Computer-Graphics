#include "viewport/painter_renderer.hpp"

#include "core/scene_templates.hpp"
#include "theme/icons.hpp"
#include "theme/theme.hpp"

#include <oxwald/scene/components.hpp>
#include <oxwald/scene/world.hpp>

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace ox::editor {

namespace {

enum class Prim { Cube, Sphere, Cylinder, Plane };

Prim primitiveOf(const Uuid& mesh) {
    const QString name = builtin::primitiveName(mesh); // also legacy ids of older scenes
    if (name == QLatin1String("Sphere")) return Prim::Sphere;
    if (name == QLatin1String("Cylinder") || name == QLatin1String("Capsule")) return Prim::Cylinder;
    if (name == QLatin1String("Plane")) return Prim::Plane;
    return Prim::Cube;
}

struct LocalFace {
    std::vector<glm::vec3> pts;
    glm::vec3 normal;
    bool edge = true; // draw outline edges (silhouette-ish)
};

const std::vector<LocalFace>& primitiveFaces(Prim p) {
    static std::vector<LocalFace> cube, sphere, cylinder, plane;
    if (cube.empty()) {
        const glm::vec3 n[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        for (const auto& nn : n) {
            glm::vec3 u = std::abs(nn.y) > 0.5f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
            glm::vec3 v = glm::cross(nn, u);
            const glm::vec3 c = nn * 0.5f;
            cube.push_back({{c - u * 0.5f - v * 0.5f, c + u * 0.5f - v * 0.5f, c + u * 0.5f + v * 0.5f, c - u * 0.5f + v * 0.5f}, nn});
        }
        const int slices = 18, stacks = 10;
        for (int i = 0; i < stacks; ++i) {
            const float t0 = kPi * float(i) / stacks, t1 = kPi * float(i + 1) / stacks;
            for (int j = 0; j < slices; ++j) {
                const float p0 = kTwoPi * float(j) / slices, p1 = kTwoPi * float(j + 1) / slices;
                auto sp = [](float t, float p) { return glm::vec3(std::sin(t) * std::cos(p), std::cos(t), std::sin(t) * std::sin(p)) * 0.5f; };
                LocalFace f;
                f.pts = {sp(t0, p0), sp(t0, p1), sp(t1, p1), sp(t1, p0)};
                f.normal = glm::normalize(sp((t0 + t1) * 0.5f, (p0 + p1) * 0.5f));
                f.edge = false;
                sphere.push_back(f);
            }
        }
        const int sides = 24;
        LocalFace top, bottom;
        for (int j = 0; j < sides; ++j) {
            const float a0 = kTwoPi * float(j) / sides, a1 = kTwoPi * float(j + 1) / sides;
            const glm::vec3 d0(std::cos(a0) * 0.5f, 0, std::sin(a0) * 0.5f), d1(std::cos(a1) * 0.5f, 0, std::sin(a1) * 0.5f);
            LocalFace f;
            f.pts = {d0 + glm::vec3(0, -0.5f, 0), d1 + glm::vec3(0, -0.5f, 0), d1 + glm::vec3(0, 0.5f, 0), d0 + glm::vec3(0, 0.5f, 0)};
            f.normal = glm::normalize(d0 + d1);
            f.edge = false;
            cylinder.push_back(f);
            top.pts.push_back(d0 + glm::vec3(0, 0.5f, 0));
            bottom.pts.insert(bottom.pts.begin(), d0 + glm::vec3(0, -0.5f, 0));
        }
        top.normal = {0, 1, 0};
        bottom.normal = {0, -1, 0};
        cylinder.push_back(top);
        cylinder.push_back(bottom);
        const int tiles = 6;
        for (int i = 0; i < tiles; ++i) {
            for (int j = 0; j < tiles; ++j) {
                const float x0 = -0.5f + float(i) / tiles, x1 = -0.5f + float(i + 1) / tiles;
                const float z0 = -0.5f + float(j) / tiles, z1 = -0.5f + float(j + 1) / tiles;
                plane.push_back({{{x0, 0, z0}, {x1, 0, z0}, {x1, 0, z1}, {x0, 0, z1}}, {0, 1, 0}, false});
            }
        }
    }
    switch (p) {
    case Prim::Sphere: return sphere;
    case Prim::Cylinder: return cylinder;
    case Prim::Plane: return plane;
    default: return cube;
    }
}

glm::vec3 basePalette(const Uuid& id, Prim prim) {
    if (prim == Prim::Plane) return {0.17f, 0.18f, 0.21f};
    static const glm::vec3 pal[] = {{0.80f, 0.81f, 0.86f}, {0.86f, 0.72f, 0.58f}, {0.58f, 0.69f, 0.88f},
                                    {0.70f, 0.82f, 0.66f}, {0.84f, 0.66f, 0.74f}, {0.79f, 0.77f, 0.66f}};
    return pal[(id.lo ^ id.hi) % 6];
}

struct Face {
    QPolygonF poly;
    float depth;
    QColor fill;
    QColor edge;
    bool selected;
    bool floor;
    Uuid owner;
};

// Convex hull (Andrew's monotone chain): the primitives are convex, so the hull of their visible faces is the
// silhouette used for the selection outline.
QPolygonF convexHull(std::vector<QPointF> pts) {
    std::sort(pts.begin(), pts.end(), [](const QPointF& a, const QPointF& b) { return a.x() < b.x() || (a.x() == b.x() && a.y() < b.y()); });
    if (pts.size() < 3) return QPolygonF(QList<QPointF>(pts.begin(), pts.end()));
    auto cross = [](const QPointF& o, const QPointF& a, const QPointF& b) {
        return (a.x() - o.x()) * (b.y() - o.y()) - (a.y() - o.y()) * (b.x() - o.x());
    };
    std::vector<QPointF> h(2 * pts.size());
    size_t k = 0;
    for (size_t i = 0; i < pts.size(); ++i) {
        while (k >= 2 && cross(h[k - 2], h[k - 1], pts[i]) <= 0) --k;
        h[k++] = pts[i];
    }
    for (size_t i = pts.size() - 1, t = k + 1; i > 0; --i) {
        while (k >= t && cross(h[k - 2], h[k - 1], pts[i - 1]) <= 0) --k;
        h[k++] = pts[i - 1];
    }
    h.resize(k - 1);
    return QPolygonF(QList<QPointF>(h.begin(), h.end()));
}

QColor toColor(glm::vec3 c, float a = 1.0f) {
    c = glm::clamp(c, 0.0f, 1.0f);
    return QColor::fromRgbF(c.r, c.g, c.b, a);
}

glm::vec3 tonemap(glm::vec3 x) { return glm::vec3(1.0f) - glm::exp(-x * 1.25f); }

// Clip a segment to the near plane (view space) and project.
bool projectSegment(const glm::mat4& view, const glm::mat4& proj, glm::vec2 size, glm::vec3 a, glm::vec3 b, QPointF& pa, QPointF& pb,
                    float nearZ) {
    glm::vec4 va = view * glm::vec4(a, 1), vb = view * glm::vec4(b, 1);
    const float n = -nearZ;
    if (va.z > n && vb.z > n) return false;
    if (va.z > n) va = glm::mix(vb, va, (vb.z - n) / (vb.z - va.z));
    if (vb.z > n) vb = glm::mix(va, vb, (va.z - n) / (va.z - vb.z));
    auto proj2 = [&](glm::vec4 v, QPointF& out) {
        glm::vec4 c = proj * v;
        if (std::abs(c.w) < 1e-6f) return false;
        glm::vec3 ndc = glm::vec3(c) / c.w;
        out = QPointF((ndc.x * 0.5 + 0.5) * size.x, (1.0 - (ndc.y * 0.5 + 0.5)) * size.y);
        return true;
    };
    return proj2(va, pa) && proj2(vb, pb);
}

QColor unpackColor(u32 c) { return QColor(int(c & 0xff), int((c >> 8) & 0xff), int((c >> 16) & 0xff), int((c >> 24) & 0xff)); }

} // namespace

void PainterViewportRenderer::render(const ViewportFrame& frame, const ViewportTarget& target) {
    QPainter& p = *target.painter;
    m_stats = {};
    const ThemePalette& tc = colors();
    const glm::vec2 size(frame.sizePx);
    const float w = size.x, h = size.y;
    const GizmoView gv = GizmoView::from(frame.camera, size);
    World* world = frame.world;
    const std::unordered_set<Uuid> selected(frame.selection.begin(), frame.selection.end());
    const std::unordered_set<Uuid> hidden(frame.hidden.begin(), frame.hidden.end());
    const QColor selColor = QColor::fromRgbF(frame.selectionColor.r, frame.selectionColor.g, frame.selectionColor.b);

    p.setRenderHint(QPainter::Antialiasing);
    // ---- background ----
    const bool wire = frame.viewMode == ViewMode::Wireframe;
    if (wire || frame.viewMode == ViewMode::Overdraw) {
        p.fillRect(QRectF(0, 0, w, h), wire ? QColor(18, 19, 24) : QColor(10, 10, 14));
    } else {
        QLinearGradient g(0, 0, 0, h);
        // horizon line from the camera pitch
        const float horizon = std::clamp(0.5f + std::tan(frame.camera.pitch) / std::tan(toRadians(frame.camera.verticalFovDeg) * 0.5f) * 0.5f, 0.0f, 1.0f);
        QColor top = tc.viewportTop, mid = mix(tc.viewportTop, QColor(120, 128, 150), 0.35), bottom = tc.viewportBottom;
        if (frame.viewMode == ViewMode::Normals) top = mid = bottom = QColor(30, 32, 40);
        g.setColorAt(0.0, top.darker(115));
        g.setColorAt(std::clamp(double(horizon) - 0.001, 0.0, 1.0), mid);
        g.setColorAt(std::clamp(double(horizon) + 0.001, 0.0, 1.0), bottom.lighter(110));
        g.setColorAt(1.0, bottom);
        p.fillRect(QRectF(0, 0, w, h), g);
    }
    if (!world) return;

    // ---- lighting environment ----
    glm::vec3 sunDir = glm::normalize(glm::vec3(-0.4f, -0.8f, -0.35f));
    glm::vec3 sunColor(1.0f);
    float ambient = 0.35f;
    glm::vec3 fogColor(0.6f, 0.7f, 0.8f);
    float fogDensity = 0.0f;
    struct PointL {
        glm::vec3 pos;
        glm::vec3 color;
        float range;
        float k;
    };
    std::vector<PointL> points;
    for (auto [e, l] : world->view<LightComponent>().each()) {
        Entity ent = world->wrap(e);
        if (!ent.activeInHierarchy()) continue;
        if (l.type == LightType::Directional) {
            sunDir = glm::normalize(ent.worldRotation() * glm::vec3(0, 0, -1));
            sunColor = l.color * std::clamp(l.intensity / 60000.0f, 0.2f, 1.6f);
        } else if (points.size() < 8) {
            points.push_back({ent.worldPosition(), l.color, std::max(0.1f, l.range), std::clamp(l.intensity / 800.0f, 0.1f, 4.0f)});
        }
    }
    for (auto [e, env] : world->view<EnvironmentComponent>().each()) {
        ambient = 0.25f + 0.15f * std::clamp(env.ambientIntensity, 0.0f, 3.0f);
        if (env.fogEnabled && frame.showFlags.fog) {
            fogColor = env.fogColor;
            fogDensity = std::clamp(env.fogDensity, 0.0f, 0.2f);
        }
        break;
    }

    // ---- geometry ----
    std::vector<Face> faces;
    faces.reserve(4096);
    const glm::vec3 camPos = frame.camera.position;
    for (auto [e, mr, wt] : world->view<MeshRendererComponent, WorldTransformComponent>().each()) {
        Entity ent = world->wrap(e);
        if (!mr.visible || !ent.activeInHierarchy()) continue;
        const Uuid id = ent.uuid();
        if (hidden.count(id)) continue;
        ++m_stats.entities;
        const Prim prim = primitiveOf(mr.mesh);
        const glm::mat4 m = ent.worldMatrix();
        const glm::mat3 nm = glm::transpose(glm::inverse(glm::mat3(m)));
        const bool sel = selected.count(id) > 0;
        const glm::vec3 base = basePalette(id, prim);
        for (const LocalFace& lf : primitiveFaces(prim)) {
            glm::vec3 n = glm::normalize(nm * lf.normal);
            glm::vec3 centroid(0.0f);
            std::vector<glm::vec3> wp;
            wp.reserve(lf.pts.size());
            for (const auto& lp : lf.pts) {
                wp.push_back(glm::vec3(m * glm::vec4(lp, 1.0f)));
                centroid += wp.back();
            }
            centroid /= float(wp.size());
            const glm::vec3 toCam = frame.camera.orthographic ? -frame.camera.forward() : camPos - centroid;
            if (prim == Prim::Plane) {
                if (glm::dot(n, toCam) < 0) n = -n; // two-sided
            } else if (glm::dot(n, toCam) <= 0.0f) {
                if (!wire) continue;
            }
            QPolygonF poly;
            bool ok = true;
            float depth = 0.0f;
            for (const auto& q : wp) {
                const glm::vec4 v = frame.view * glm::vec4(q, 1);
                if (v.z > -frame.camera.nearPlane) {
                    ok = false;
                    break;
                }
                depth += -v.z;
                QPointF s;
                if (!gv.project(q, s)) {
                    ok = false;
                    break;
                }
                poly << s;
            }
            if (!ok) continue;
            depth /= float(wp.size());
            glm::vec3 col;
            const float diff = std::max(0.0f, glm::dot(n, -sunDir));
            glm::vec3 lit = glm::vec3(ambient) * glm::mix(glm::vec3(0.55f, 0.6f, 0.75f), glm::vec3(1.0f), 0.5f + 0.5f * n.y) +
                            sunColor * diff * (frame.showFlags.shadows ? 0.95f : 0.85f);
            for (const auto& pl : points) {
                const glm::vec3 L = pl.pos - centroid;
                const float d = glm::length(L);
                if (d > pl.range) continue;
                const float att = std::pow(1.0f - d / pl.range, 2.0f);
                lit += pl.color * std::max(0.0f, glm::dot(n, L / std::max(d, 1e-3f))) * att * pl.k * 0.9f;
            }
            switch (frame.viewMode) {
            case ViewMode::Unlit:
            case ViewMode::BufferBaseColor: col = base; break;
            case ViewMode::LightingOnly: col = tonemap(glm::vec3(0.75f) * lit); break;
            case ViewMode::Normals: col = n * 0.5f + 0.5f; break;
            case ViewMode::BufferDepth: col = glm::vec3(std::clamp(1.0f - depth / 60.0f, 0.0f, 1.0f)); break;
            default: col = tonemap(base * lit); break;
            }
            if (fogDensity > 0.0f && (frame.viewMode == ViewMode::Lit)) {
                const float f = 1.0f - std::exp(-fogDensity * depth);
                col = glm::mix(col, fogColor * 0.85f, f);
            }
            Face f;
            f.owner = id;
            f.poly = poly;
            f.depth = depth;
            f.selected = sel;
            f.floor = prim == Prim::Plane;
            if (frame.viewMode == ViewMode::Overdraw) {
                f.fill = QColor(255, 120, 40, 40);
                f.edge = Qt::transparent;
            } else if (wire) {
                f.fill = Qt::transparent;
                f.edge = sel ? selColor : toColor(base * 0.9f + 0.1f, 0.85f);
            } else {
                f.fill = toColor(col);
                f.edge = lf.edge ? toColor(col * 0.7f, 0.55f) : f.fill;
            }
            faces.push_back(f);
        }
    }
    std::unordered_map<Uuid, std::vector<QPointF>> selectedHulls;
    for (const Face& f : faces) {
        if (f.selected && !f.floor) {
            auto& v = selectedHulls[f.owner];
            v.insert(v.end(), f.poly.begin(), f.poly.end());
        }
    }
    std::stable_sort(faces.begin(), faces.end(), [](const Face& a, const Face& b) {
        if (a.floor != b.floor) return a.floor;
        return a.depth > b.depth;
    });
    m_stats.faces = int(faces.size());

    auto drawFace = [&](const Face& f) {
        p.setBrush(f.fill);
        if (wire) p.setPen(QPen(f.edge, f.selected ? 1.6 : 1.0));
        else p.setPen(QPen(f.edge, 0.8));
        p.drawPolygon(f.poly);
    };
    size_t i = 0;
    for (; i < faces.size() && faces[i].floor; ++i) drawFace(faces[i]);

    // ---- grid (over floors, under objects) ----
    if (frame.showFlags.grid && !isBufferView(frame.viewMode)) {
        const float cell = m_gridSize > 0.0f ? m_gridSize : 1.0f;
        const float camH = std::abs(camPos.y);
        const float extent = std::clamp(camH * 8.0f + 30.0f, 30.0f, 400.0f);
        const int lines = std::min(400, int(extent / cell));
        const glm::vec3 c0(std::round(camPos.x / cell) * cell, 0, std::round(camPos.z / cell) * cell);
        QColor gc = m_gridColor.isValid() ? m_gridColor : tc.gridMinor;
        for (int k = -lines; k <= lines; ++k) {
            for (int axis = 0; axis < 2; ++axis) {
                const float off = float(k) * cell;
                glm::vec3 a = c0, b = c0;
                const float coord = axis == 0 ? c0.x + off : c0.z + off;
                if (axis == 0) {
                    a.x = b.x = coord;
                    a.z -= extent;
                    b.z += extent;
                } else {
                    a.z = b.z = coord;
                    a.x -= extent;
                    b.x += extent;
                }
                const bool isAxis = std::abs(coord) < cell * 0.01f;
                const bool major = std::abs(std::fmod(std::round(coord / cell), 10.0f)) < 0.5f;
                // fade with distance of the line from the camera
                const float dist = std::abs(axis == 0 ? coord - camPos.x : coord - camPos.z);
                const float fade = std::clamp(1.0f - dist / extent, 0.0f, 1.0f);
                if (fade <= 0.02f) continue;
                // split long lines in segments so near-plane clipping and fading stay smooth
                const int segs = 8;
                for (int s = 0; s < segs; ++s) {
                    const glm::vec3 sa = glm::mix(a, b, float(s) / segs), sb = glm::mix(a, b, float(s + 1) / segs);
                    const float segDist = glm::length(glm::vec2((sa + sb).x * 0.5f - camPos.x, (sa + sb).z * 0.5f - camPos.z));
                    const float sf = fade * std::clamp(1.0f - segDist / extent, 0.0f, 1.0f);
                    if (sf <= 0.02f) continue;
                    QPointF pa, pb;
                    if (!projectSegment(frame.view, frame.projection, size, sa, sb, pa, pb, frame.camera.nearPlane)) continue;
                    QColor col = gc;
                    float width = 1.0f;
                    if (isAxis) {
                        col = axis == 0 ? Theme::axisColor(2) : Theme::axisColor(0);
                        col.setAlpha(int(200 * sf));
                        width = 1.5f;
                    } else {
                        col.setAlpha(int(std::min(255.0f, float(gc.alpha()) * (major ? 2.0f : 1.0f) * sf)));
                    }
                    p.setPen(QPen(col, width));
                    p.drawLine(pa, pb);
                    ++m_stats.lines;
                }
            }
        }
    }

    for (; i < faces.size(); ++i) drawFace(faces[i]);

    // ---- selection outline (silhouette of each selected primitive) ----
    if (!wire) {
        p.setBrush(Qt::NoBrush);
        for (auto& [id, pts] : selectedHulls) {
            const QPolygonF hull = convexHull(pts);
            p.setPen(QPen(withAlpha(selColor, 70), 5.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawPolygon(hull);
            p.setPen(QPen(selColor, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawPolygon(hull);
        }
    }

    // ---- debug draw lines ----
    if (frame.lines && frame.showFlags.debugDraw) {
        auto drawLines = [&](std::span<const DebugVertex> verts, float width) {
            for (size_t k = 0; k + 1 < verts.size(); k += 2) {
                QPointF pa, pb;
                if (!projectSegment(frame.view, frame.projection, size, verts[k].position, verts[k + 1].position, pa, pb, frame.camera.nearPlane)) continue;
                p.setPen(QPen(unpackColor(verts[k].color), width, Qt::SolidLine, Qt::RoundCap));
                p.drawLine(pa, pb);
                ++m_stats.lines;
            }
        };
        drawLines(frame.lines->depthTestedLines(), 1.2f);
        drawLines(frame.lines->overlayLines(), 1.6f);
        p.setPen(tc.text);
        for (const auto& t : frame.lines->texts()) {
            QPointF s;
            if (gv.project(t.position, s)) {
                p.setPen(unpackColor(t.color));
                p.drawText(s, QString::fromStdString(t.text));
            }
        }
    }

    // ---- light / camera icons & shapes ----
    auto drawIcon = [&](glm::vec3 pos, const QString& icon, bool sel, QColor tint) {
        QPointF s;
        if (!gv.project(pos, s)) return;
        const glm::vec4 v = frame.view * glm::vec4(pos, 1);
        if (v.z > -frame.camera.nearPlane) return;
        const double r = 13.0;
        p.setPen(QPen(sel ? selColor : QColor(255, 255, 255, 60), sel ? 2.0 : 1.0));
        p.setBrush(QColor(16, 17, 22, 190));
        p.drawEllipse(s, r, r);
        const QPixmap pm = Icons::pixmap(icon, 16 * 2, tint);
        p.drawPixmap(QRectF(s.x() - 8, s.y() - 8, 16, 16), pm, QRectF(0, 0, pm.width(), pm.height()));
    };
    auto line3 = [&](glm::vec3 a, glm::vec3 b, QColor c, float width = 1.2f, Qt::PenStyle style = Qt::SolidLine) {
        QPointF pa, pb;
        if (!projectSegment(frame.view, frame.projection, size, a, b, pa, pb, frame.camera.nearPlane)) return;
        p.setPen(QPen(c, width, style, Qt::RoundCap));
        p.drawLine(pa, pb);
    };
    auto circle3 = [&](glm::vec3 c, glm::vec3 u, glm::vec3 v, float r, QColor col) {
        glm::vec3 prev = c + u * r;
        for (int s = 1; s <= 48; ++s) {
            const float a = kTwoPi * float(s) / 48.0f;
            const glm::vec3 q = c + (u * std::cos(a) + v * std::sin(a)) * r;
            line3(prev, q, col, 1.0f);
            prev = q;
        }
    };
    if (!isBufferView(frame.viewMode)) {
        for (auto [e, l] : world->view<LightComponent>().each()) {
            Entity ent = world->wrap(e);
            const Uuid id = ent.uuid();
            if (hidden.count(id) || !frame.showFlags.lights) continue;
            const bool sel = selected.count(id) > 0;
            const glm::vec3 pos = ent.worldPosition();
            const glm::quat rot = ent.worldRotation();
            const glm::vec3 fwd = rot * glm::vec3(0, 0, -1);
            const QColor lc = toColor(glm::clamp(l.color, 0.2f, 1.0f));
            QColor shape = sel ? selColor : withAlpha(lc, 120);
            if (l.type == LightType::Directional) {
                line3(pos, pos + fwd * 2.0f, shape, sel ? 2.0f : 1.4f);
                for (int k = 0; k < 4; ++k) {
                    const glm::vec3 off = rot * glm::vec3(std::cos(k * kHalfPi) * 0.3f, std::sin(k * kHalfPi) * 0.3f, 0);
                    line3(pos + off, pos + off + fwd * 1.4f, withAlpha(shape, 90));
                }
            } else if (l.type == LightType::Spot && sel) {
                const float len = std::min(l.range, 8.0f);
                const float r = std::tan(toRadians(l.outerConeAngle)) * len;
                const glm::vec3 u = rot * glm::vec3(1, 0, 0), v = rot * glm::vec3(0, 1, 0);
                circle3(pos + fwd * len, u, v, r, shape);
                for (int k = 0; k < 4; ++k) line3(pos, pos + fwd * len + (u * std::cos(k * kHalfPi) + v * std::sin(k * kHalfPi)) * r, shape);
            } else if (l.type == LightType::Point && sel) {
                circle3(pos, {1, 0, 0}, {0, 0, 1}, l.range, withAlpha(shape, 150));
                circle3(pos, {1, 0, 0}, {0, 1, 0}, l.range, withAlpha(shape, 90));
                circle3(pos, {0, 0, 1}, {0, 1, 0}, l.range, withAlpha(shape, 90));
            } else if (l.type == LightType::AreaRect) {
                const glm::vec3 u = rot * glm::vec3(l.areaSize.x * 0.5f, 0, 0), v = rot * glm::vec3(0, l.areaSize.y * 0.5f, 0);
                line3(pos - u - v, pos + u - v, shape);
                line3(pos + u - v, pos + u + v, shape);
                line3(pos + u + v, pos - u + v, shape);
                line3(pos - u + v, pos - u - v, shape);
                line3(pos, pos + fwd, shape);
            }
            if (frame.showFlags.icons) {
                const QString icon = l.type == LightType::Directional ? QStringLiteral("sun")
                                     : l.type == LightType::Spot     ? QStringLiteral("light-spot")
                                     : l.type == LightType::AreaRect ? QStringLiteral("light-area")
                                                                     : QStringLiteral("light-point");
                drawIcon(pos, icon, sel, lc.lightness() < 90 ? QColor(255, 230, 160) : lc);
            }
        }
        for (auto [e, cam] : world->view<CameraComponent>().each()) {
            Entity ent = world->wrap(e);
            const Uuid id = ent.uuid();
            if (hidden.count(id) || !frame.showFlags.cameras || frame.playMode) continue;
            const bool sel = selected.count(id) > 0;
            const glm::vec3 pos = ent.worldPosition();
            const glm::quat rot = ent.worldRotation();
            // small frustum
            const float d = 0.9f, hh = std::tan(toRadians(cam.verticalFov) * 0.5f) * d, ww = hh * 16.0f / 9.0f;
            const glm::vec3 fwd = rot * glm::vec3(0, 0, -1), u = rot * glm::vec3(1, 0, 0), v = rot * glm::vec3(0, 1, 0);
            const glm::vec3 c = pos + fwd * d;
            const glm::vec3 corners[4] = {c - u * ww - v * hh, c + u * ww - v * hh, c + u * ww + v * hh, c - u * ww + v * hh};
            const QColor col = sel ? selColor : QColor(200, 205, 220, 150);
            for (int k = 0; k < 4; ++k) {
                line3(pos, corners[k], col, sel ? 1.6f : 1.0f);
                line3(corners[k], corners[(k + 1) % 4], col, sel ? 1.6f : 1.0f);
            }
            line3(c + v * hh * 1.15f - u * ww * 0.3f, c + v * hh * 1.5f, col);
            line3(c + v * hh * 1.5f, c + v * hh * 1.15f + u * ww * 0.3f, col);
            if (frame.showFlags.icons) drawIcon(pos, QStringLiteral("camera"), sel, QColor(220, 225, 240));
        }
        // empties / environment as small markers when selected
        for (const Uuid& id : frame.selection) {
            Entity ent = world->find(id);
            if (!ent || ent.has<MeshRendererComponent>() || ent.has<LightComponent>() || ent.has<CameraComponent>()) continue;
            const glm::vec3 pos = ent.worldPosition();
            const float s = 0.3f;
            line3(pos - glm::vec3(s, 0, 0), pos + glm::vec3(s, 0, 0), selColor, 1.5f);
            line3(pos - glm::vec3(0, s, 0), pos + glm::vec3(0, s, 0), selColor, 1.5f);
            line3(pos - glm::vec3(0, 0, s), pos + glm::vec3(0, 0, s), selColor, 1.5f);
        }
    }

    if (isBufferView(frame.viewMode) && frame.viewMode != ViewMode::BufferBaseColor && frame.viewMode != ViewMode::BufferDepth) {
        QRectF box(w / 2 - 190, h / 2 - 34, 380, 68);
        p.setPen(QPen(tc.overlayBorder, 1));
        p.setBrush(tc.overlay);
        p.drawRoundedRect(box, 10, 10);
        p.setPen(tc.text);
        p.drawText(box.adjusted(12, 8, -12, -30), Qt::AlignCenter, QObject::tr("%1 buffer visualisation").arg(viewModeName(frame.viewMode)));
        p.setPen(tc.textFaint);
        p.drawText(box.adjusted(12, 30, -12, -8), Qt::AlignCenter, QObject::tr("Available when the GPU renderer (render module) is active"));
    }
}

// ---- picking ------------------------------------------------------------------------------------------------

AABB entityBounds(World& world, Entity e) {
    (void)world;
    AABB box;
    const glm::mat4 m = e.worldMatrix();
    if (auto* mr = e.tryGet<MeshRendererComponent>()) {
        const Prim prim = primitiveOf(mr->mesh);
        const AABB local = prim == Prim::Plane ? AABB::fromMinMax({-0.5f, -0.01f, -0.5f}, {0.5f, 0.01f, 0.5f})
                                               : AABB::fromMinMax(glm::vec3(-0.5f), glm::vec3(0.5f));
        return local.transformed(m);
    }
    const glm::vec3 p = e.worldPosition();
    return AABB::fromCenterExtents(p, glm::vec3(0.5f));
}

namespace {
bool contains(const std::vector<Uuid>& v, const Uuid& id) { return std::find(v.begin(), v.end(), id) != v.end(); }
} // namespace

std::optional<PickHit> pickEntity(World& world, const GizmoView& view, QPointF pixel, const std::vector<Uuid>& hidden,
                                  const std::vector<Uuid>& locked) {
    const Ray ray = view.ray(pixel);
    std::optional<PickHit> best;
    world.forEachInHierarchy([&](entt::entity h) {
        Entity e = world.wrap(h);
        const Uuid id = e.uuid();
        if (contains(hidden, id) || contains(locked, id) || !e.activeInHierarchy()) return;
        std::optional<float> t;
        if (auto* mr = e.tryGet<MeshRendererComponent>()) {
            if (!mr->visible) return;
            const glm::mat4 m = e.worldMatrix();
            const glm::mat4 inv = glm::inverse(m);
            Ray local;
            local.origin = glm::vec3(inv * glm::vec4(ray.origin, 1.0f));
            const glm::vec3 dir = glm::vec3(inv * glm::vec4(ray.direction, 0.0f));
            const float dl = glm::length(dir);
            if (dl < 1e-8f) return;
            local.direction = dir / dl;
            const Prim prim = primitiveOf(mr->mesh);
            std::optional<float> lt;
            if (prim == Prim::Sphere) lt = intersectRaySphere(local, Sphere{glm::vec3(0.0f), 0.5f});
            else if (prim == Prim::Plane) lt = intersectRayAABB(local, AABB::fromMinMax({-0.5f, -0.02f, -0.5f}, {0.5f, 0.02f, 0.5f}));
            else lt = intersectRayAABB(local, AABB::fromMinMax(glm::vec3(-0.5f), glm::vec3(0.5f)));
            if (lt) t = glm::length(glm::vec3(m * glm::vec4(local.at(*lt), 1.0f)) - ray.origin);
            // planes lose against anything else on top of them
            if (t && prim == Prim::Plane) *t += 1e-3f;
        } else if (e.has<LightComponent>() || e.has<CameraComponent>()) {
            QPointF s;
            const glm::vec3 p = e.worldPosition();
            if (view.project(p, s) && std::hypot(s.x() - pixel.x(), s.y() - pixel.y()) < 14.0) t = glm::length(p - ray.origin) - 0.5f;
        }
        if (t && *t >= 0.0f && (!best || *t < best->distance)) best = PickHit{id, *t};
    });
    return best;
}

std::vector<Uuid> pickRect(World& world, const GizmoView& view, const QRectF& rect, const std::vector<Uuid>& hidden,
                           const std::vector<Uuid>& locked) {
    std::vector<Uuid> out;
    world.forEachInHierarchy([&](entt::entity h) {
        Entity e = world.wrap(h);
        const Uuid id = e.uuid();
        if (contains(hidden, id) || contains(locked, id)) return;
        if (!e.has<MeshRendererComponent>() && !e.has<LightComponent>() && !e.has<CameraComponent>()) return;
        if (auto* mr = e.tryGet<MeshRendererComponent>(); mr && primitiveOf(mr->mesh) == Prim::Plane) return;
        QPointF s;
        if (view.project(e.worldPosition(), s) && rect.contains(s)) out.push_back(id);
    });
    return out;
}

} // namespace ox::editor
