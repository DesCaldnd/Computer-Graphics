#include "theme/theme.hpp"

#include "theme/icons.hpp"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStyleFactory>

namespace ox::editor {

QString cssColor(const QColor& c) {
    if (c.alpha() == 255) return c.name(QColor::HexRgb);
    return QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
}

QColor mix(const QColor& a, const QColor& b, qreal t) {
    return QColor::fromRgbF(float(a.redF() + (b.redF() - a.redF()) * t), float(a.greenF() + (b.greenF() - a.greenF()) * t),
                            float(a.blueF() + (b.blueF() - a.blueF()) * t),
                            float(a.alphaF() + (b.alphaF() - a.alphaF()) * t));
}

QColor withAlpha(const QColor& c, int alpha) {
    QColor r = c;
    r.setAlpha(alpha);
    return r;
}

Theme& Theme::instance() {
    static Theme theme;
    return theme;
}

QColor Theme::axisColor(int axis) {
    const auto& p = instance().palette();
    return axis == 0 ? p.axisX : axis == 1 ? p.axisY : p.axisZ;
}

QFont Theme::monoFont() {
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    f.setPixelSize(std::max(10, instance().fontSize() - 1));
    return f;
}

void Theme::buildPalette() {
    ThemePalette& p = m_palette;
    const QColor a = m_accentBase.isValid() ? m_accentBase : defaultAccent();
    if (m_mode == ThemeMode::Dark) {
        p.bg0 = QColor("#101116");
        p.bg1 = QColor("#16171D");
        p.bg2 = QColor("#1C1E26");
        p.bg3 = QColor("#252833");
        p.bg4 = QColor("#2E3240");
        p.bgAlt = QColor("#191A21");
        p.input = QColor("#121318");
        p.inputFocus = QColor("#0E0F13");
        p.border = QColor("#262833");
        p.borderStrong = QColor("#323543");
        p.borderHover = QColor("#3F4354");
        p.text = QColor("#E7E8EF");
        p.textDim = QColor("#A2A6B6");
        p.textFaint = QColor("#6C7184");
        p.onAccent = QColor("#FFFFFF");
        p.accent = a;
        p.accentHover = a.lighter(115);
        p.accentPressed = a.darker(115);
        p.accentSubtle = withAlpha(a, 46);
        p.accentSelected = withAlpha(a, 82);
        p.accentBorder = withAlpha(a, 140);
        p.accentText = a.lighter(135);
        p.success = QColor("#3DD68C");
        p.warning = QColor("#F5B642");
        p.error = QColor("#FF5C6C");
        p.info = QColor("#4EA8FF");
        p.axisX = QColor("#F0506E");
        p.axisY = QColor("#86D45A");
        p.axisZ = QColor("#4F8DFF");
        p.cardHeader = QColor("#21232D");
        p.overlay = QColor(18, 19, 25, 210);
        p.overlayBorder = QColor(255, 255, 255, 22);
        p.scroll = QColor("#353948");
        p.scrollHover = QColor("#4A4F62");
        p.viewportTop = QColor("#2A2E3D");
        p.viewportBottom = QColor("#15161C");
        p.gridMinor = QColor(255, 255, 255, 26);
        p.gridMajor = QColor(255, 255, 255, 48);
    } else {
        p.bg0 = QColor("#E6E8EF");
        p.bg1 = QColor("#F4F5F9");
        p.bg2 = QColor("#FFFFFF");
        p.bg3 = QColor("#E9EBF2");
        p.bg4 = QColor("#DCDFE9");
        p.bgAlt = QColor("#EEF0F5");
        p.input = QColor("#FFFFFF");
        p.inputFocus = QColor("#FFFFFF");
        p.border = QColor("#D6D9E3");
        p.borderStrong = QColor("#C7CBD8");
        p.borderHover = QColor("#AEB3C4");
        p.text = QColor("#1B1D25");
        p.textDim = QColor("#525667");
        p.textFaint = QColor("#8B90A3");
        p.onAccent = QColor("#FFFFFF");
        p.accent = a;
        p.accentHover = a.lighter(110);
        p.accentPressed = a.darker(112);
        p.accentSubtle = withAlpha(a, 34);
        p.accentSelected = withAlpha(a, 60);
        p.accentBorder = withAlpha(a, 120);
        p.accentText = a.darker(115);
        p.success = QColor("#16A765");
        p.warning = QColor("#C98500");
        p.error = QColor("#E03445");
        p.info = QColor("#1F7AE0");
        p.axisX = QColor("#E0284D");
        p.axisY = QColor("#4CA82A");
        p.axisZ = QColor("#2367E8");
        p.cardHeader = QColor("#F7F8FB");
        p.overlay = QColor(255, 255, 255, 220);
        p.overlayBorder = QColor(0, 0, 0, 28);
        p.scroll = QColor("#C5C9D6");
        p.scrollHover = QColor("#A9AEBF");
        p.viewportTop = QColor("#AEB8CE");
        p.viewportBottom = QColor("#E4E7EE");
        p.gridMinor = QColor(0, 0, 0, 26);
        p.gridMajor = QColor(0, 0, 0, 56);
    }
}

namespace {

void savePng(const QString& dir, const QString& name, int size, const std::function<void(QPainter&, qreal)>& draw) {
    for (int scale : {1, 2}) {
        QImage img(size * scale, size * scale, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(scale, scale);
        draw(p, size);
        p.end();
        img.save(dir + "/" + name + (scale == 2 ? "@2x.png" : ".png"));
    }
}

} // namespace

void Theme::renderStyleImages(const QString& dir) {
    QDir().mkpath(dir);
    const ThemePalette& c = m_palette;
    auto box = [&](QColor fill, QColor border, bool check, bool mixed, QColor mark) {
        return [=](QPainter& p, qreal s) {
            QRectF r(1.0, 1.0, s - 2.0, s - 2.0);
            p.setPen(QPen(border, 1.2));
            p.setBrush(fill);
            p.drawRoundedRect(r, 4, 4);
            if (check) {
                QPainterPath path;
                path.moveTo(s * 0.27, s * 0.52);
                path.lineTo(s * 0.44, s * 0.68);
                path.lineTo(s * 0.74, s * 0.34);
                p.setPen(QPen(mark, 1.9, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                p.setBrush(Qt::NoBrush);
                p.drawPath(path);
            }
            if (mixed) {
                p.setPen(QPen(mark, 2.0, Qt::SolidLine, Qt::RoundCap));
                p.drawLine(QPointF(s * 0.3, s * 0.5), QPointF(s * 0.7, s * 0.5));
            }
        };
    };
    savePng(dir, "check-off", 16, box(c.input, c.borderHover, false, false, c.onAccent));
    savePng(dir, "check-off-hover", 16, box(c.input, c.accent, false, false, c.onAccent));
    savePng(dir, "check-on", 16, box(c.accent, c.accent, true, false, c.onAccent));
    savePng(dir, "check-mixed", 16, box(c.accent, c.accent, false, true, c.onAccent));
    savePng(dir, "check-disabled", 16, box(c.bg2, c.border, false, false, c.textFaint));
    savePng(dir, "check-on-disabled", 16, box(c.bg4, c.border, true, false, c.textFaint));
    savePng(dir, "radio-off", 16, [&](QPainter& p, qreal s) {
        p.setPen(QPen(c.borderHover, 1.2));
        p.setBrush(c.input);
        p.drawEllipse(QRectF(1, 1, s - 2, s - 2));
    });
    savePng(dir, "radio-on", 16, [&](QPainter& p, qreal s) {
        p.setPen(QPen(c.accent, 1.2));
        p.setBrush(c.accent);
        p.drawEllipse(QRectF(1, 1, s - 2, s - 2));
        p.setBrush(c.onAccent);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QRectF(s * 0.32, s * 0.32, s * 0.36, s * 0.36));
    });
    for (const char* name : {"chevron-down", "chevron-right", "close"}) {
        for (int scale : {1, 2}) {
            QPixmap pm = Icons::pixmap(name, 16 * scale, c.textDim);
            pm.setDevicePixelRatio(1.0);
            pm.save(dir + "/" + name + (scale == 2 ? "@2x.png" : ".png"));
        }
    }
}

QStringList Theme::unresolvedTokens() const {
    QStringList out;
    static const QRegularExpression re(QStringLiteral("@([A-Za-z0-9]+)@"));
    auto it = re.globalMatch(m_styleSheet);
    while (it.hasNext()) out << it.next().captured(1);
    out.removeDuplicates();
    return out;
}

void Theme::apply(ThemeMode mode, const QColor& accent, int fontSizePx) {
    m_mode = mode;
    m_accentBase = accent.isValid() ? accent : defaultAccent();
    m_fontSize = std::clamp(fontSizePx, 10, 20);
    ++m_generation;
    buildPalette();
    Icons::clearCache();

    const QString imgDir = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                               .filePath(QStringLiteral("oxwald-editor-theme-%1-%2")
                                             .arg(QCoreApplication::applicationPid())
                                             .arg(m_generation));
    renderStyleImages(imgDir);

    const ThemePalette& c = m_palette;
    m_tokens.clear();
    auto tok = [&](const char* k, const QColor& v) { m_tokens.insert(QString::fromLatin1(k), cssColor(v)); };
    tok("bg0", c.bg0);
    tok("bg1", c.bg1);
    tok("bg2", c.bg2);
    tok("bg3", c.bg3);
    tok("bg4", c.bg4);
    tok("bgAlt", c.bgAlt);
    tok("input", c.input);
    tok("inputFocus", c.inputFocus);
    tok("border", c.border);
    tok("borderStrong", c.borderStrong);
    tok("borderHover", c.borderHover);
    tok("text", c.text);
    tok("textDim", c.textDim);
    tok("textFaint", c.textFaint);
    tok("accent", c.accent);
    tok("accentHover", c.accentHover);
    tok("accentPressed", c.accentPressed);
    tok("accentSubtle", c.accentSubtle);
    tok("accentSelected", c.accentSelected);
    tok("accentBorder", c.accentBorder);
    tok("accentText", c.accentText);
    tok("onAccent", c.onAccent);
    tok("success", c.success);
    tok("successSubtle", withAlpha(c.success, 40));
    tok("warning", c.warning);
    tok("warningSubtle", withAlpha(c.warning, 30));
    tok("warningBorder", withAlpha(c.warning, 110));
    tok("error", c.error);
    tok("info", c.info);
    tok("cardHeader", c.cardHeader);
    tok("overlay", c.overlay);
    tok("overlayBorder", c.overlayBorder);
    tok("scroll", c.scroll);
    tok("scrollHover", c.scrollHover);
    m_tokens.insert("fontSize", QString::number(m_fontSize));
    m_tokens.insert("fontSmall", QString::number(m_fontSize - 2));
    m_tokens.insert("fontTitle", QString::number(m_fontSize + 2));
    m_tokens.insert("fontHeading", QString::number(m_fontSize + 9));
    m_tokens.insert("iconDir", QDir::fromNativeSeparators(imgDir));

    QFile f(QStringLiteral(":/theme/editor.qss"));
    QString qss;
    if (f.open(QIODevice::ReadOnly)) qss = QString::fromUtf8(f.readAll());
    for (auto it = m_tokens.cbegin(); it != m_tokens.cend(); ++it) {
        qss.replace(QLatin1Char('@') + it.key() + QLatin1Char('@'), it.value());
    }
    m_styleSheet = qss;

    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
        if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) app->setStyle(fusion);
        QPalette pal;
        pal.setColor(QPalette::Window, c.bg1);
        pal.setColor(QPalette::WindowText, c.text);
        pal.setColor(QPalette::Base, c.input);
        pal.setColor(QPalette::AlternateBase, c.bgAlt);
        pal.setColor(QPalette::ToolTipBase, c.bg3);
        pal.setColor(QPalette::ToolTipText, c.text);
        pal.setColor(QPalette::PlaceholderText, c.textFaint);
        pal.setColor(QPalette::Text, c.text);
        pal.setColor(QPalette::Button, c.bg3);
        pal.setColor(QPalette::ButtonText, c.text);
        pal.setColor(QPalette::BrightText, c.onAccent);
        pal.setColor(QPalette::Highlight, c.accent);
        pal.setColor(QPalette::HighlightedText, c.onAccent);
        pal.setColor(QPalette::Link, c.accentText);
        pal.setColor(QPalette::Light, c.bg4);
        pal.setColor(QPalette::Midlight, c.bg3);
        pal.setColor(QPalette::Mid, c.border);
        pal.setColor(QPalette::Dark, c.bg0);
        pal.setColor(QPalette::Shadow, QColor(0, 0, 0, 160));
        pal.setColor(QPalette::Disabled, QPalette::Text, c.textFaint);
        pal.setColor(QPalette::Disabled, QPalette::WindowText, c.textFaint);
        pal.setColor(QPalette::Disabled, QPalette::ButtonText, c.textFaint);
        app->setPalette(pal);
        QFont font = QApplication::font();
        font.setPixelSize(m_fontSize);
        app->setFont(font);
        app->setStyleSheet(m_styleSheet);
    }
    Q_EMIT changed();
}

} // namespace ox::editor
