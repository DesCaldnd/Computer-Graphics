#pragma once

#include "integration/gameplay_tools.hpp"

#include <oxwald/core/uuid.hpp>

#include <QObject>

namespace ox::editor {

// Active viewport tool (shared by the inspector component extensions that toggle it and the viewport).
enum class ViewportTool { Transform, SplinePoints, TerrainSculpt };

class ToolState : public QObject {
    Q_OBJECT
public:
    [[nodiscard]] ViewportTool tool() const { return m_tool; }
    [[nodiscard]] const Uuid& target() const { return m_target; }
    void setTool(ViewportTool tool, const Uuid& target = {}) {
        if (tool == m_tool && target == m_target) return;
        m_tool = tool;
        m_target = target;
        splinePoint = -1;
        Q_EMIT changed();
    }
    void reset() { setTool(ViewportTool::Transform); }

    int splinePoint = -1;      // selected control point (SplinePoints)
    SculptSettings sculpt;     // TerrainSculpt brush

Q_SIGNALS:
    void changed();

private:
    ViewportTool m_tool = ViewportTool::Transform;
    Uuid m_target;
};

} // namespace ox::editor
