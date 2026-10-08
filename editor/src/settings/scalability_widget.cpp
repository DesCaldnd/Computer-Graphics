#include "settings/scalability_widget.hpp"

#include "core/editor_context.hpp"
#include "theme/icons.hpp"
#include "theme/theme.hpp"
#include "widgets/widgets.hpp"

#include <oxwald/core/scalability.hpp>

#include <QApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

namespace ox::editor {

QString groupDisplayName(Scalability g) {
    switch (g) {
    case Scalability::ViewDistance: return QObject::tr("View Distance");
    case Scalability::AntiAliasing: return QObject::tr("Anti-Aliasing");
    case Scalability::Shadows: return QObject::tr("Shadows");
    case Scalability::GlobalIllumination: return QObject::tr("Global Illumination");
    case Scalability::Reflections: return QObject::tr("Reflections");
    case Scalability::PostProcess: return QObject::tr("Post Processing");
    case Scalability::Textures: return QObject::tr("Textures");
    case Scalability::Effects: return QObject::tr("Effects");
    case Scalability::Foliage: return QObject::tr("Foliage");
    case Scalability::Shading: return QObject::tr("Shading");
    case Scalability::Volumetrics: return QObject::tr("Volumetrics");
    case Scalability::RayTracing: return QObject::tr("Ray Tracing");
    default: return {};
    }
}

QString groupIcon(Scalability g) {
    switch (g) {
    case Scalability::ViewDistance: return QStringLiteral("view-distance");
    case Scalability::AntiAliasing: return QStringLiteral("aa");
    case Scalability::Shadows: return QStringLiteral("shadow");
    case Scalability::GlobalIllumination: return QStringLiteral("sun");
    case Scalability::Reflections: return QStringLiteral("sphere");
    case Scalability::PostProcess: return QStringLiteral("post-process");
    case Scalability::Textures: return QStringLiteral("texture");
    case Scalability::Effects: return QStringLiteral("effects");
    case Scalability::Foliage: return QStringLiteral("foliage");
    case Scalability::Shading: return QStringLiteral("shading");
    case Scalability::Volumetrics: return QStringLiteral("volumetrics");
    case Scalability::RayTracing: return QStringLiteral("rtx");
    default: return QStringLiteral("settings");
    }
}

QString levelDisplayName(QualityLevel l) {
    switch (l) {
    case QualityLevel::Low: return QObject::tr("Low");
    case QualityLevel::Medium: return QObject::tr("Medium");
    case QualityLevel::High: return QObject::tr("High");
    case QualityLevel::Ultra: return QObject::tr("Ultra");
    default: return QObject::tr("Custom");
    }
}

namespace {
QStringList levelLabels() {
    return {levelDisplayName(QualityLevel::Low), levelDisplayName(QualityLevel::Medium), levelDisplayName(QualityLevel::High),
            levelDisplayName(QualityLevel::Ultra)};
}
QLabel* customBadge(QWidget* parent) {
    auto* l = new QLabel(QObject::tr("Custom"), parent);
    l->setStyleSheet(QStringLiteral("QLabel{background:%1;color:%2;border-radius:4px;padding:1px 6px;font-weight:600;}")
                         .arg(cssColor(withAlpha(colors().warning, 40)), cssColor(colors().warning)));
    l->setToolTip(QObject::tr("Some cvars of this group were changed individually and no longer match a preset level"));
    l->setVisible(false);
    return l;
}
} // namespace

ScalabilityWidget::ScalabilityWidget(EditorContext* ctx, bool compact, QWidget* parent) : QWidget(parent), m_ctx(ctx), m_compact(compact) {
    setObjectName(QStringLiteral("ScalabilityWidget"));
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(compact ? 6 : 8);

    // header: overall + auto-detect
    auto* top = new QFrame(this);
    top->setProperty("role", "card");
    auto* tl = new QGridLayout(top);
    tl->setContentsMargins(12, 10, 12, 10);
    tl->setHorizontalSpacing(12);
    auto* oIcon = new QLabel(top);
    oIcon->setPixmap(Icons::get(QStringLiteral("overall"), Icons::Tint::Accent).pixmap(QSize(18, 18)));
    auto* oTitle = new QLabel(tr("Overall Quality"), top);
    QFont f = oTitle->font();
    f.setWeight(QFont::DemiBold);
    f.setPixelSize(Theme::instance().fontSize() + 1);
    oTitle->setFont(f);
    m_overall = new SegmentedControl(levelLabels(), top);
    m_overall->setObjectName(QStringLiteral("scalability:Overall"));
    m_overallCustom = customBadge(top);
    auto* detect = new QPushButton(Icons::get(QStringLiteral("wand"), Icons::Tint::OnAccent), tr("Auto-Detect"), top);
    detect->setObjectName(QStringLiteral("AutoDetectButton"));
    detect->setProperty("role", "primary");
    detect->setToolTip(tr("Benchmark this machine and choose a level per group"));
    tl->addWidget(oIcon, 0, 0);
    tl->addWidget(oTitle, 0, 1);
    tl->addWidget(m_overallCustom, 0, 2);
    tl->setColumnStretch(3, 1);
    tl->addWidget(m_overall, 0, 4);
    tl->addWidget(detect, 0, 5);
    m_result = new QLabel(top);
    m_result->setProperty("role", "faint");
    m_result->setWordWrap(true);
    m_result->setVisible(false);
    tl->addWidget(m_result, 1, 1, 1, 5);
    lay->addWidget(top);
    connect(m_overall, &SegmentedControl::activated, this, [this](int i) { apply(-1, QualityLevel(i)); });
    connect(detect, &QPushButton::clicked, this, [this] { autoDetect(); });

    // group rows
    auto* table = new QFrame(this);
    table->setProperty("role", "card");
    auto* grid = new QVBoxLayout(table);
    grid->setContentsMargins(4, 4, 4, 4);
    grid->setSpacing(0);
    for (usize gi = 0; gi < kScalabilityGroupCount; ++gi) {
        const auto g = Scalability(gi);
        Row& r = m_rows[gi];
        auto* rowW = new QWidget(table);
        auto* rl = new QHBoxLayout(rowW);
        rl->setContentsMargins(8, compact ? 3 : 5, 8, compact ? 3 : 5);
        rl->setSpacing(10);
        r.expand = new QToolButton(rowW);
        r.expand->setIcon(Icons::get(QStringLiteral("chevron-right")));
        r.expand->setIconSize(QSize(12, 12));
        r.expand->setAutoRaise(true);
        r.expand->setToolTip(tr("Show the console variables this group sets"));
        r.expand->setVisible(!compact);
        auto* icon = new QLabel(rowW);
        icon->setPixmap(Icons::get(groupIcon(g)).pixmap(QSize(16, 16)));
        auto* name = new QLabel(groupDisplayName(g), rowW);
        name->setMinimumWidth(compact ? 120 : 150);
        r.custom = customBadge(rowW);
        r.control = new SegmentedControl(levelLabels(), rowW);
        r.control->setObjectName(QStringLiteral("scalability:%1").arg(QString::fromLatin1(scalability::groupName(g).data())));
        rl->addWidget(r.expand);
        rl->addWidget(icon);
        rl->addWidget(name);
        rl->addWidget(r.custom);
        rl->addStretch(1);
        rl->addWidget(r.control);
        grid->addWidget(rowW);
        if (g == Scalability::RayTracing) {
            const RenderingCaps caps = ctx->services().caps().caps();
            if (!caps.rayTracingSupported) {
                name->setToolTip(caps.rayTracingUnavailableReason);
                icon->setToolTip(caps.rayTracingUnavailableReason);
                name->setText(groupDisplayName(g) + QStringLiteral("  ⓘ"));
            }
        }
        r.details = new QFrame(table);
        r.details->setVisible(false);
        r.details->setStyleSheet(QStringLiteral("QFrame{background:%1;border-radius:6px;}").arg(cssColor(colors().input)));
        r.detailsGrid = new QGridLayout(r.details);
        r.detailsGrid->setContentsMargins(36, 6, 12, 8);
        r.detailsGrid->setHorizontalSpacing(14);
        r.detailsGrid->setVerticalSpacing(3);
        grid->addWidget(r.details);
        if (gi + 1 < kScalabilityGroupCount) grid->addWidget(makeHairline(table));
        connect(r.control, &SegmentedControl::activated, this, [this, gi](int i) { apply(int(gi), QualityLevel(i)); });
        connect(r.expand, &QToolButton::clicked, this, [this, gi] {
            Row& row = m_rows[gi];
            const bool show = !row.details->isVisible();
            row.details->setVisible(show);
            row.expand->setIcon(Icons::get(show ? QStringLiteral("chevron-down") : QStringLiteral("chevron-right")));
            if (show) fillDetails(Scalability(gi));
        });
    }
    lay->addWidget(table);
    refresh();
}

void ScalabilityWidget::apply(int group, QualityLevel level) {
    if (m_commit) m_commit(group, level);
    else if (group < 0) scalability::setOverall(level);
    else scalability::setGroup(Scalability(group), level);
    refresh();
    Q_EMIT levelsChanged();
}

void ScalabilityWidget::fillDetails(Scalability g) {
    Row& r = m_rows[size_t(g)];
    while (QLayoutItem* it = r.detailsGrid->takeAt(0)) {
        if (it->widget()) it->widget()->deleteLater();
        delete it;
    }
    const QualityLevel cur = scalability::currentLevel(g);
    const QStringList heads = {tr("Console variable"), levelDisplayName(QualityLevel::Low), levelDisplayName(QualityLevel::Medium),
                               levelDisplayName(QualityLevel::High), levelDisplayName(QualityLevel::Ultra), tr("Current")};
    for (int c = 0; c < heads.size(); ++c) {
        auto* h = makeSectionLabel(heads[c], r.details);
        r.detailsGrid->addWidget(h, 0, c);
    }
    int row = 1;
    const auto list = scalability::cvars(g);
    if (list.empty()) {
        auto* l = new QLabel(tr("No cvars are bound to this group yet."), r.details);
        l->setProperty("role", "faint");
        r.detailsGrid->addWidget(l, 1, 0, 1, 6);
        return;
    }
    for (ICVar* cv : list) {
        auto* name = new QLabel(QString::fromStdString(cv->name()), r.details);
        name->setFont(Theme::monoFont());
        name->setToolTip(QString::fromStdString(cv->description()));
        r.detailsGrid->addWidget(name, row, 0);
        for (int lv = 0; lv < 4; ++lv) {
            auto* v = new QLabel(QString::fromStdString(cv->levelString(QualityLevel(lv))), r.details);
            v->setFont(Theme::monoFont());
            v->setAlignment(Qt::AlignCenter);
            if (int(cur) == lv) {
                v->setStyleSheet(QStringLiteral("QLabel{background:%1;color:%2;border-radius:4px;padding:0 6px;font-weight:600;}")
                                     .arg(cssColor(colors().accentSubtle), cssColor(colors().text)));
            } else {
                v->setProperty("role", "dim");
            }
            r.detailsGrid->addWidget(v, row, 1 + lv);
        }
        auto* curV = new QLabel(QString::fromStdString(cv->toString()), r.details);
        curV->setFont(Theme::monoFont());
        curV->setAlignment(Qt::AlignCenter);
        if (cur == QualityLevel::Custom && !cv->matchesLevel(QualityLevel(std::max(0, int(cur))))) {
            curV->setStyleSheet(QStringLiteral("QLabel{color:%1;font-weight:600;}").arg(cssColor(colors().warning)));
        }
        r.detailsGrid->addWidget(curV, row, 5);
        ++row;
    }
}

void ScalabilityWidget::refresh() {
    const QualityLevel overall = scalability::overallLevel();
    m_overall->setCurrent(overall == QualityLevel::Custom ? -1 : int(overall));
    m_overallCustom->setVisible(overall == QualityLevel::Custom);
    for (usize gi = 0; gi < kScalabilityGroupCount; ++gi) {
        const QualityLevel l = scalability::currentLevel(Scalability(gi));
        Row& r = m_rows[gi];
        r.control->setCurrent(l == QualityLevel::Custom ? -1 : int(l));
        r.custom->setVisible(l == QualityLevel::Custom);
        if (r.details->isVisible()) fillDetails(Scalability(gi));
    }
}

QString ScalabilityWidget::autoDetect() {
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const RenderingCaps caps = m_ctx->services().caps().caps();
    const BenchmarkResult res = m_ctx->services().benchmark().run(caps);
    QApplication::restoreOverrideCursor();
    for (usize gi = 0; gi < kScalabilityGroupCount; ++gi) apply(int(gi), res.levels[gi]);
    // Recommended AA / upscaler of the renderer's Auto quality (render::recommendedSettings).
    auto setCVar = [](const char* name, int v) {
        if (v < 0) return;
        if (ICVar* c = CVarRegistry::instance().find(name)) c->setFromJson(nlohmann::json(v), CVarSource::Config);
    };
    setCVar("r.AntiAliasing", res.antiAliasing);
    setCVar("r.Upscaler", res.upscaler);
    setCVar("r.Upscaler.Quality", res.upscalerQuality);
    QStringList summary;
    for (usize gi = 0; gi < kScalabilityGroupCount; ++gi) summary << groupDisplayName(Scalability(gi)) + QStringLiteral(" ") + levelDisplayName(res.levels[gi]);
    const QString text = tr("Detected with %1: %2").arg(m_ctx->services().benchmark().name(), res.details);
    m_result->setText(text);
    m_result->setToolTip(summary.join(QLatin1Char('\n')));
    m_result->setVisible(true);
    refresh();
    return text;
}

} // namespace ox::editor
