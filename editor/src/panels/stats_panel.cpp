#include "panels/stats_panel.hpp"

#include "core/editor_context.hpp"
#include "theme/icons.hpp"
#include "theme/theme.hpp"
#include "viewport/viewport_panel.hpp"
#include "widgets/widgets.hpp"

#include <QGridLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QScrollArea>
#include <QVBoxLayout>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <cstdio>
#include <unistd.h>
#endif

namespace ox::editor {

quint64 processMemoryBytes() {
#if defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        return info.resident_size;
    }
#elif defined(__linux__)
    long rss = 0;
    if (FILE* f = std::fopen("/proc/self/statm", "r")) {
        long size = 0;
        if (std::fscanf(f, "%ld %ld", &size, &rss) != 2) rss = 0;
        std::fclose(f);
    }
    return quint64(rss) * quint64(sysconf(_SC_PAGESIZE));
#endif
    return 0;
}

FrameGraph::FrameGraph(QWidget* parent) : QWidget(parent) { setMinimumHeight(90); }

void FrameGraph::push(double ms) {
    m_samples.push_back(ms);
    if (m_samples.size() > m_capacity) m_samples.remove(0, m_samples.size() - m_capacity);
    update();
}

double FrameGraph::average() const {
    if (m_samples.isEmpty()) return 0.0;
    double s = 0;
    for (double v : m_samples) s += v;
    return s / double(m_samples.size());
}

double FrameGraph::maximum() const {
    double m = 0;
    for (double v : m_samples) m = std::max(m, v);
    return m;
}

void FrameGraph::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const ThemePalette& c = colors();
    QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QPen(c.border, 1));
    p.setBrush(c.input);
    p.drawRoundedRect(r, 8, 8);
    const double top = std::max(40.0, maximum() * 1.15);
    auto yOf = [&](double ms) { return r.bottom() - 6 - (ms / top) * (r.height() - 12); };
    QFont f = Theme::monoFont();
    f.setPixelSize(10);
    p.setFont(f);
    for (double guide : {16.67, 33.33}) {
        const double y = yOf(guide);
        p.setPen(QPen(withAlpha(guide < 20 ? c.success : c.warning, 90), 1, Qt::DashLine));
        p.drawLine(QPointF(r.left() + 6, y), QPointF(r.right() - 6, y));
        p.setPen(withAlpha(c.textFaint, 200));
        p.drawText(QPointF(r.right() - 46, y - 3), guide < 20 ? QStringLiteral("60 fps") : QStringLiteral("30 fps"));
    }
    if (m_samples.size() < 2) return;
    QPainterPath line, area;
    const double dx = (r.width() - 12) / double(m_capacity - 1);
    const double x0 = r.right() - 6 - dx * double(m_samples.size() - 1);
    for (int i = 0; i < m_samples.size(); ++i) {
        const QPointF pt(x0 + dx * i, yOf(m_samples[i]));
        if (i == 0) {
            line.moveTo(pt);
            area.moveTo(QPointF(pt.x(), r.bottom() - 6));
        } else {
            line.lineTo(pt);
        }
        area.lineTo(pt);
    }
    area.lineTo(QPointF(r.right() - 6, r.bottom() - 6));
    area.closeSubpath();
    QLinearGradient g(0, r.top(), 0, r.bottom());
    g.setColorAt(0, withAlpha(c.accent, 110));
    g.setColorAt(1, withAlpha(c.accent, 8));
    p.setPen(Qt::NoPen);
    p.setBrush(g);
    p.drawPath(area);
    p.setPen(QPen(c.accentText, 1.6));
    p.setBrush(Qt::NoBrush);
    p.drawPath(line);
}

namespace {
QWidget* statTile(const QString& icon, const QString& title, QLabel*& value, QWidget* parent) {
    auto* f = new QFrame(parent);
    f->setProperty("role", "card");
    auto* l = new QVBoxLayout(f);
    l->setContentsMargins(10, 8, 10, 8);
    l->setSpacing(2);
    auto* head = new QHBoxLayout();
    auto* ic = new QLabel(f);
    ic->setPixmap(Icons::get(icon, Icons::Tint::Accent).pixmap(QSize(14, 14)));
    auto* t = new QLabel(title.toUpper(), f);
    t->setProperty("role", "section");
    head->addWidget(ic);
    head->addWidget(t, 1);
    l->addLayout(head);
    value = new QLabel(QStringLiteral("—"), f);
    QFont vf = value->font();
    vf.setPixelSize(Theme::instance().fontSize() + 7);
    vf.setWeight(QFont::DemiBold);
    value->setFont(vf);
    l->addWidget(value);
    return f;
}
} // namespace

StatsPanel::StatsPanel(EditorContext* ctx, ViewportPanel* viewport, QWidget* parent) : QWidget(parent), m_ctx(ctx), m_viewport(viewport) {
    setObjectName(QStringLiteral("StatsPanel"));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* host = new QWidget(scroll);
    auto* lay = new QVBoxLayout(host);
    lay->setContentsMargins(10, 10, 10, 10);
    lay->setSpacing(10);
    auto* tiles = new QGridLayout();
    tiles->setSpacing(8);
    tiles->addWidget(statTile(QStringLiteral("speedometer"), tr("Frame rate"), m_fps, host), 0, 0);
    tiles->addWidget(statTile(QStringLiteral("clock"), tr("Frame time"), m_ms, host), 0, 1);
    tiles->addWidget(statTile(QStringLiteral("memory"), tr("Memory (RSS)"), m_mem, host), 1, 0);
    tiles->addWidget(statTile(QStringLiteral("entity"), tr("Entities"), m_entities, host), 1, 1);
    lay->addLayout(tiles);
    lay->addWidget(makeSectionLabel(tr("Frame time (ms)"), host));
    m_graph = new FrameGraph(host);
    lay->addWidget(m_graph);
    m_undo = new QLabel(host);
    m_undo->setProperty("role", "faint");
    lay->addWidget(m_undo);
    lay->addWidget(makeSectionLabel(tr("Systems (play mode, CPU)"), host));
    auto* sysHost = new QWidget(host);
    m_systems = new QGridLayout(sysHost);
    m_systems->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(sysHost);
    lay->addWidget(makeSectionLabel(tr("GPU passes"), host));
    auto* passHost = new QWidget(host);
    m_passes = new QGridLayout(passHost);
    m_passes->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(passHost);
    lay->addStretch(1);
    scroll->setWidget(host);
    outer->addWidget(scroll);
    if (viewport) connect(viewport, &ViewportPanel::frameRendered, m_graph, &FrameGraph::push);
    connect(&m_timer, &QTimer::timeout, this, &StatsPanel::refresh);
    m_timer.start(500);
    refresh();
}

void StatsPanel::fillTable(QGridLayout* grid, const QVector<QPair<QString, double>>& rows, const QString& emptyText) {
    while (QLayoutItem* it = grid->takeAt(0)) {
        if (it->widget()) it->widget()->deleteLater();
        delete it;
    }
    if (rows.isEmpty()) {
        auto* l = new QLabel(emptyText);
        l->setProperty("role", "faint");
        l->setWordWrap(true);
        grid->addWidget(l, 0, 0, 1, 3);
        return;
    }
    double maxv = 0.001;
    for (const auto& r : rows) maxv = std::max(maxv, r.second);
    int row = 0;
    for (const auto& r : rows) {
        auto* name = new QLabel(r.first);
        auto* bar = new QProgressBar();
        bar->setRange(0, 1000);
        bar->setValue(int(r.second / maxv * 1000));
        bar->setFixedHeight(6);
        auto* val = new QLabel(QStringLiteral("%1 ms").arg(r.second, 0, 'f', 3));
        val->setProperty("role", "dim");
        val->setFont(Theme::monoFont());
        grid->addWidget(name, row, 0);
        grid->addWidget(bar, row, 1);
        grid->addWidget(val, row, 2, Qt::AlignRight);
        ++row;
    }
    grid->setColumnStretch(1, 1);
}

void StatsPanel::refresh() {
    if (!isVisible()) return;
    const double fps = m_viewport ? m_viewport->fps() : 0.0;
    m_fps->setText(QStringLiteral("%1").arg(fps, 0, 'f', 0));
    m_ms->setText(QStringLiteral("%1 ms").arg(m_graph->average(), 0, 'f', 2));
    m_mem->setText(formatBytes(processMemoryBytes()));
    m_entities->setText(QString::number(m_ctx->world().entityCount()));
    m_undo->setText(tr("Undo stack: %1 commands · max frame %2 ms").arg(m_ctx->undoStack().count()).arg(m_graph->maximum(), 0, 'f', 2));
    QVector<QPair<QString, double>> sys;
    if (auto* s = m_ctx->play().scheduler()) {
        for (int ph = 0; ph < int(SystemPhase::Count); ++ph) {
            for (ISystem* system : s->systems(SystemPhase(ph))) sys.push_back({qs(system->name()), s->lastTimeMs(system->name())});
        }
    }
    fillTable(m_systems, sys, tr("Start Play or Simulate to profile gameplay systems."));
    QVector<QPair<QString, double>> passes;
    if (m_viewport && m_viewport->renderer()) {
        for (const auto& p : m_viewport->renderer()->passTimings()) passes.push_back({qs(p.name), p.milliseconds});
    }
    fillTable(m_passes, passes, tr("The active viewport renderer (%1) does not report GPU pass timings. They appear here once the render module's renderer is active.")
                                    .arg(m_viewport ? m_viewport->rendererName() : QString()));
}

} // namespace ox::editor
