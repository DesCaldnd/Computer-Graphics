#pragma once

#include <QTimer>
#include <QVector>
#include <QWidget>

class QLabel;
class QGridLayout;
class QVBoxLayout;

namespace ox::editor {

class EditorContext;
class ViewportPanel;

// Area chart of the last N frame times with 60/30 FPS guide lines.
class FrameGraph : public QWidget {
    Q_OBJECT
public:
    explicit FrameGraph(QWidget* parent = nullptr);
    void push(double ms);
    [[nodiscard]] QSize sizeHint() const override { return {320, 96}; }
    [[nodiscard]] double average() const;
    [[nodiscard]] double maximum() const;

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QVector<double> m_samples;
    int m_capacity = 240;
};

quint64 processMemoryBytes();

// Frame timing graph, per-system CPU times (play mode), per-pass GPU times (when the renderer reports them),
// memory and scene counters.
class StatsPanel : public QWidget {
    Q_OBJECT
public:
    StatsPanel(EditorContext* ctx, ViewportPanel* viewport, QWidget* parent = nullptr);
    [[nodiscard]] FrameGraph* graph() const { return m_graph; }

private:
    void refresh();
    void fillTable(QGridLayout* grid, const QVector<QPair<QString, double>>& rows, const QString& emptyText);

    EditorContext* m_ctx;
    ViewportPanel* m_viewport;
    FrameGraph* m_graph;
    QLabel* m_fps;
    QLabel* m_ms;
    QLabel* m_mem;
    QLabel* m_entities;
    QLabel* m_undo;
    QGridLayout* m_systems;
    QGridLayout* m_passes;
    QTimer m_timer;
};

} // namespace ox::editor
