#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QString>
#include <QStringList>

namespace ox::editor {

// The editor's own 24×24 stroke icon set (resources/icons/*.svg, stroke="currentColor"), recoloured at load.
// Icons returned by get() follow the theme: they repaint in the current palette for every QIcon mode
// (normal = dim text, active/hover = text, selected = on-accent, disabled = faint, checked = accent).
class Icons {
public:
    enum class Tint { Theme, Accent, Text, Dim, Faint, Success, Warning, Error, Info, OnAccent };

    [[nodiscard]] static QIcon get(const QString& name, Tint tint = Tint::Theme);
    [[nodiscard]] static QIcon fixed(const QString& name, const QColor& color);
    [[nodiscard]] static QPixmap pixmap(const QString& name, int sizePx, const QColor& color);
    [[nodiscard]] static bool exists(const QString& name);
    [[nodiscard]] static QStringList names();
    // Maps component icon hints / asset types to icon names ("light" -> "light", unknown -> "component").
    [[nodiscard]] static QString forComponent(const QString& iconHint, const QString& componentName = {});
    [[nodiscard]] static QString forAssetType(const QString& type);
    [[nodiscard]] static QColor tintColor(Tint tint);
    static void clearCache();
    [[nodiscard]] static QByteArray svgData(const QString& name);
};

} // namespace ox::editor
