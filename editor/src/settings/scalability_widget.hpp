#pragma once

#include <oxwald/core/cvar.hpp>

#include <QWidget>

#include <array>
#include <functional>

class QLabel;
class QPushButton;
class QGridLayout;
class QToolButton;

namespace ox::editor {

class EditorContext;
class SegmentedControl;

// UE-like scalability grid: rows = groups, segmented Low/Medium/High/Ultra, an "Overall" row, Custom badges,
// per-group cvar details (value per level, current value), and Auto-Detect (benchmark from EditorServices).
// Changes apply live. `commit` lets a host dialog route changes through its undo stack.
class ScalabilityWidget : public QWidget {
    Q_OBJECT
public:
    using Commit = std::function<void(int group /* -1 = overall */, QualityLevel level)>;

    ScalabilityWidget(EditorContext* ctx, bool compact, QWidget* parent = nullptr);
    void setCommit(Commit c) { m_commit = std::move(c); }
    void refresh();
    // Runs the benchmark and applies the detected levels. Returns the summary text.
    QString autoDetect();
    [[nodiscard]] SegmentedControl* overallControl() const { return m_overall; }
    [[nodiscard]] SegmentedControl* groupControl(Scalability g) const { return m_rows[size_t(g)].control; }

Q_SIGNALS:
    void levelsChanged();

private:
    struct Row {
        SegmentedControl* control = nullptr;
        QLabel* custom = nullptr;
        QToolButton* expand = nullptr;
        QWidget* details = nullptr;
        QGridLayout* detailsGrid = nullptr;
    };
    void apply(int group, QualityLevel level);
    void fillDetails(Scalability g);

    EditorContext* m_ctx;
    bool m_compact;
    SegmentedControl* m_overall = nullptr;
    QLabel* m_overallCustom = nullptr;
    QLabel* m_result = nullptr;
    std::array<Row, kScalabilityGroupCount> m_rows{};
    Commit m_commit;
};

QString groupDisplayName(Scalability g);
QString groupIcon(Scalability g);
QString levelDisplayName(QualityLevel l);

} // namespace ox::editor
