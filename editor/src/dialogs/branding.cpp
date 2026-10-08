#include "dialogs/branding.hpp"

#include "core/editor_context.hpp"
#include "theme/icons.hpp"
#include "theme/theme.hpp"
#include "widgets/widgets.hpp"

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScreen>
#include <QSvgRenderer>
#include <QVBoxLayout>

namespace ox::editor {

QStringList integratedModules() {
    QStringList m{QStringLiteral("core"), QStringLiteral("scene")};
#if OX_EDITOR_HAS_RHI
    m << QStringLiteral("rhi");
#endif
#if OX_EDITOR_HAS_ASSETS
    m << QStringLiteral("assets");
#endif
#if OX_EDITOR_HAS_RENDER
    m << QStringLiteral("render");
#endif
#if OX_EDITOR_HAS_GAMEPLAY
    m << QStringLiteral("gameplay");
#endif
#if OX_EDITOR_HAS_RUNTIME
    m << QStringLiteral("runtime");
#endif
#if OX_EDITOR_HAS_ASYNC
    m << QStringLiteral("async");
#endif
    return m;
}

QStringList pendingModules() {
    QStringList all{QStringLiteral("rhi"), QStringLiteral("assets"), QStringLiteral("render"), QStringLiteral("gameplay"), QStringLiteral("runtime"), QStringLiteral("async")};
    const QStringList have = integratedModules();
    QStringList out;
    for (const auto& m : all) {
        if (!have.contains(m)) out << m;
    }
    return out;
}

QPixmap logoPixmap(int size) {
    QSvgRenderer r(QStringLiteral(":/logo.svg"));
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    r.render(&p);
    return pm;
}

qreal paintWordmark(QPainter& p, QPointF tl, qreal h, bool light) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    QSvgRenderer r(QStringLiteral(":/logo.svg"));
    r.render(&p, QRectF(tl, QSizeF(h, h)));
    QFont bold = QApplication::font();
    bold.setPixelSize(int(h * 0.62));
    bold.setWeight(QFont::Bold);
    bold.setLetterSpacing(QFont::PercentageSpacing, 98);
    QFont thin = bold;
    thin.setWeight(QFont::Light);
    const qreal x0 = tl.x() + h * 1.25;
    const QFontMetricsF fb(bold), ft(thin);
    const qreal base = tl.y() + h * 0.5 + fb.capHeight() * 0.5;
    p.setFont(bold);
    p.setPen(light ? QColor(24, 24, 30) : QColor(245, 245, 250));
    p.drawText(QPointF(x0, base), QStringLiteral("Oxwald"));
    const qreal x1 = x0 + fb.horizontalAdvance(QStringLiteral("Oxwald"));
    QLinearGradient g(x1, 0, x1 + ft.horizontalAdvance(QStringLiteral("Engine")), 0);
    g.setColorAt(0, QColor("#A48BFF"));
    g.setColorAt(1, QColor("#5FB6FF"));
    p.setFont(thin);
    p.setPen(QPen(QBrush(g), 1));
    p.drawText(QPointF(x1 + 2, base), QStringLiteral("Engine"));
    const qreal w = x1 + 2 + ft.horizontalAdvance(QStringLiteral("Engine")) - tl.x();
    p.restore();
    return w;
}

// ---- splash -------------------------------------------------------------------------------------------------

SplashScreen::SplashScreen() : QWidget(nullptr, Qt::SplashScreen | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint) {
    setAttribute(Qt::WA_TranslucentBackground);
    resize(620, 360);
    if (QScreen* s = QGuiApplication::primaryScreen()) move(s->geometry().center() - rect().center());
}

void SplashScreen::setMessage(const QString& msg, int progressPercent) {
    m_message = msg;
    m_progress = progressPercent;
    repaint();
    QCoreApplication::processEvents();
}

void SplashScreen::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QRectF r = QRectF(rect()).adjusted(8, 8, -8, -8);
    QPainterPath card;
    card.addRoundedRect(r, 18, 18);
    QLinearGradient bg(r.topLeft(), r.bottomRight());
    bg.setColorAt(0, QColor("#17142A"));
    bg.setColorAt(0.55, QColor("#111219"));
    bg.setColorAt(1, QColor("#0C0D12"));
    p.fillPath(card, bg);
    // soft glow
    QRadialGradient glow(r.left() + r.width() * 0.8, r.top() + r.height() * 0.1, r.width() * 0.7);
    glow.setColorAt(0, QColor(124, 92, 255, 90));
    glow.setColorAt(1, QColor(124, 92, 255, 0));
    p.fillPath(card, glow);
    // isometric line art
    p.save();
    p.setClipPath(card);
    p.setPen(QPen(QColor(255, 255, 255, 14), 1));
    for (int i = -20; i < 40; ++i) {
        const qreal x = r.left() + i * 26;
        p.drawLine(QPointF(x, r.bottom()), QPointF(x + r.height() * 1.2, r.top()));
        p.drawLine(QPointF(x, r.top()), QPointF(x + r.height() * 1.2, r.bottom()));
    }
    p.restore();
    p.setPen(QPen(QColor(255, 255, 255, 30), 1));
    p.drawPath(card);
    paintWordmark(p, QPointF(r.left() + 40, r.top() + 104), 64);
    QFont f = font();
    f.setPixelSize(13);
    p.setFont(f);
    p.setPen(QColor(170, 172, 190));
    p.drawText(QPointF(r.left() + 42, r.top() + 204), tr("Editor %1  ·  Vulkan · Ray tracing · Lua").arg(QStringLiteral(OX_EDITOR_VERSION)));
    // progress
    const QRectF bar(r.left() + 40, r.bottom() - 52, r.width() - 80, 4);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 25));
    p.drawRoundedRect(bar, 2, 2);
    QLinearGradient pg(bar.topLeft(), bar.topRight());
    pg.setColorAt(0, QColor("#7C5CFF"));
    pg.setColorAt(1, QColor("#5FB6FF"));
    p.setBrush(pg);
    p.drawRoundedRect(QRectF(bar.left(), bar.top(), bar.width() * std::clamp(m_progress, 0, 100) / 100.0, bar.height()), 2, 2);
    p.setPen(QColor(150, 152, 170));
    f.setPixelSize(12);
    p.setFont(f);
    p.drawText(QRectF(bar.left(), bar.bottom() + 8, bar.width(), 20), Qt::AlignLeft | Qt::AlignTop, m_message);
    p.drawText(QRectF(bar.left(), bar.bottom() + 8, bar.width(), 20), Qt::AlignRight | Qt::AlignTop, QStringLiteral("© 2026 OxwaldEngine"));
}

// ---- about --------------------------------------------------------------------------------------------------

namespace {
class WordmarkWidget : public QWidget {
public:
    using QWidget::QWidget;
    QSize sizeHint() const override { return {420, 120}; }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QPainterPath path;
        path.addRoundedRect(QRectF(rect()), 12, 12);
        QLinearGradient bg(0, 0, width(), height());
        bg.setColorAt(0, QColor("#1B1633"));
        bg.setColorAt(1, QColor("#0F1016"));
        p.fillPath(path, bg);
        QRadialGradient glow(width() * 0.85, 0, width() * 0.6);
        glow.setColorAt(0, QColor(124, 92, 255, 80));
        glow.setColorAt(1, QColor(124, 92, 255, 0));
        p.fillPath(path, glow);
        paintWordmark(p, QPointF(24, height() / 2.0 - 26), 52);
    }
};
} // namespace

AboutDialog::AboutDialog(EditorContext* ctx, QWidget* parent) : QDialog(parent) {
    (void)ctx;
    setObjectName(QStringLiteral("OxDialog"));
    setWindowTitle(tr("About OxwaldEditor"));
    setFixedWidth(520);
    auto* l = new QVBoxLayout(this);
    l->setContentsMargins(20, 20, 20, 16);
    l->setSpacing(12);
    auto* mark = new WordmarkWidget(this);
    mark->setMinimumHeight(120);
    l->addWidget(mark);
    auto* ver = new QLabel(tr("<b>OxwaldEditor %1</b> — the editor of OxwaldEngine, a Vulkan game engine with raster and ray traced rendering.")
                               .arg(QStringLiteral(OX_EDITOR_VERSION)),
                           this);
    ver->setWordWrap(true);
    l->addWidget(ver);
    auto* grid = new QFrame(this);
    grid->setProperty("role", "card");
    auto* gl = new QVBoxLayout(grid);
    gl->setContentsMargins(12, 10, 12, 10);
    auto row = [&](const QString& k, const QString& v) {
        auto* h = new QHBoxLayout();
        auto* a = new QLabel(k, grid);
        a->setProperty("role", "dim");
        auto* b = new QLabel(v, grid);
        b->setTextInteractionFlags(Qt::TextSelectableByMouse);
        b->setWordWrap(true);
        h->addWidget(a);
        h->addStretch(1);
        h->addWidget(b);
        gl->addLayout(h);
    };
    row(tr("Qt"), QString::fromLatin1(qVersion()));
    row(tr("Compiler"), QStringLiteral(__VERSION__).left(40));
    row(tr("Integrated modules"), integratedModules().join(QStringLiteral(", ")));
    row(tr("Pending modules"), pendingModules().isEmpty() ? tr("none") : pendingModules().join(QStringLiteral(", ")));
    l->addWidget(grid);
    auto* foot = new QHBoxLayout();
    auto* credits = new QLabel(tr("Built with Qt, EnTT, glm, Jolt, Lua, miniaudio, Recast/Detour, ENet."), this);
    credits->setProperty("role", "faint");
    credits->setWordWrap(true);
    auto* ok = new QPushButton(tr("Close"), this);
    ok->setProperty("role", "primary");
    connect(ok, &QPushButton::clicked, this, &QDialog::accept);
    foot->addWidget(credits, 1);
    foot->addWidget(ok);
    l->addLayout(foot);
}

} // namespace ox::editor
