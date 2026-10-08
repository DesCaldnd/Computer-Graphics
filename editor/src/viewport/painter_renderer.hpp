#pragma once

#include "viewport/gizmo.hpp"
#include "viewport/viewport_renderer.hpp"

#include <QColor>

#include <oxwald/scene/world.hpp>

#include <optional>

namespace ox::editor {

// Software viewport renderer used until the render module provides a GPU IViewportRenderer: sky gradient,
// fading ground grid, flat-shaded placeholder primitives (painter's algorithm), light/camera icons and shapes,
// debug-draw lines and selection outlines. Also used for headless screenshots.
class PainterViewportRenderer final : public IViewportRenderer {
public:
    [[nodiscard]] QString name() const override { return QStringLiteral("Software preview (QPainter)"); }
    [[nodiscard]] bool usesPainter() const override { return true; }
    void render(const ViewportFrame& frame, const ViewportTarget& target) override;

    struct Stats {
        int faces = 0;
        int lines = 0;
        int entities = 0;
    };
    [[nodiscard]] const Stats& lastStats() const { return m_stats; }
    void setGridColor(const QColor& c) { m_gridColor = c; }
    void setGridSize(float s) { m_gridSize = s; }

private:
    Stats m_stats;
    QColor m_gridColor;
    float m_gridSize = 1.0f;
};

// CPU picking against primitive bounds and icon discs; used when the renderer has no ID buffer.
struct PickHit {
    Uuid id;
    float distance = 0.0f;
};
std::optional<PickHit> pickEntity(World& world, const GizmoView& view, QPointF pixel, const std::vector<Uuid>& hidden,
                                  const std::vector<Uuid>& locked);
std::vector<Uuid> pickRect(World& world, const GizmoView& view, const QRectF& rect, const std::vector<Uuid>& hidden,
                           const std::vector<Uuid>& locked);
// World-space bounds of an entity's visual (primitive or icon size), for focusing.
AABB entityBounds(World& world, Entity e);

} // namespace ox::editor
