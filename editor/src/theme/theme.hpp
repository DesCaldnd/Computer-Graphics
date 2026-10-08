#pragma once

#include <QColor>
#include <QFont>
#include <QHash>
#include <QObject>
#include <QString>

class QApplication;

namespace ox::editor {

enum class ThemeMode { Dark, Light };

// Colour tokens shared by the style sheet, custom-painted widgets and the viewport.
struct ThemePalette {
    QColor bg0, bg1, bg2, bg3, bg4, bgAlt;
    QColor input, inputFocus;
    QColor border, borderStrong, borderHover;
    QColor text, textDim, textFaint;
    QColor accent, accentHover, accentPressed, accentSubtle, accentSelected, accentBorder, accentText, onAccent;
    QColor success, warning, error, info;
    QColor axisX, axisY, axisZ;
    QColor cardHeader, overlay, overlayBorder, scroll, scrollHover;
    QColor viewportTop, viewportBottom, gridMinor, gridMajor;
};

// Process-wide UI theme (dark/light + accent + font size). Applies QPalette + generated style sheet and
// renders the small tinted images the style sheet references (check boxes, arrows) into a cache directory.
class Theme : public QObject {
    Q_OBJECT
public:
    static Theme& instance();

    void apply(ThemeMode mode, const QColor& accent, int fontSizePx = 13);
    void reapply() { apply(m_mode, m_accentBase, m_fontSize); }

    [[nodiscard]] ThemeMode mode() const { return m_mode; }
    [[nodiscard]] const ThemePalette& palette() const { return m_palette; }
    [[nodiscard]] QColor accent() const { return m_palette.accent; }
    [[nodiscard]] int fontSize() const { return m_fontSize; }
    // Bumped on every apply(): icon caches compare against it.
    [[nodiscard]] int generation() const { return m_generation; }
    [[nodiscard]] const QString& styleSheet() const { return m_styleSheet; }
    // Tokens left unreplaced in the generated style sheet (tests assert this is empty).
    [[nodiscard]] QStringList unresolvedTokens() const;
    [[nodiscard]] QHash<QString, QString> tokens() const { return m_tokens; }

    static QColor defaultAccent() { return QColor(0x7C, 0x5C, 0xFF); }
    static QColor axisColor(int axis);
    static QFont monoFont();

Q_SIGNALS:
    void changed();

private:
    Theme() = default;
    void buildPalette();
    void renderStyleImages(const QString& dir);

    ThemeMode m_mode = ThemeMode::Dark;
    QColor m_accentBase = defaultAccent();
    int m_fontSize = 13;
    int m_generation = 0;
    ThemePalette m_palette;
    QHash<QString, QString> m_tokens;
    QString m_styleSheet;
};

inline const ThemePalette& colors() { return Theme::instance().palette(); }

// Hex string "#rrggbb" or "rgba(r,g,b,a)" for style sheets.
QString cssColor(const QColor& c);
QColor mix(const QColor& a, const QColor& b, qreal t);
QColor withAlpha(const QColor& c, int alpha);

} // namespace ox::editor
