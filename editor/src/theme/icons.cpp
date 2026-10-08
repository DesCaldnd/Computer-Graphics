#include "theme/icons.hpp"

#include "theme/theme.hpp"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QHash>
#include <QIconEngine>
#include <QMutex>
#include <QPainter>
#include <QSvgRenderer>

namespace ox::editor {

namespace {

QMutex g_mutex;
QHash<QString, QByteArray> g_svg;
QHash<QString, QPixmap> g_pixmaps;

QByteArray loadSvg(const QString& name) {
    QMutexLocker lock(&g_mutex);
    auto it = g_svg.find(name);
    if (it != g_svg.end()) return *it;
    QFile f(QStringLiteral(":/icons/%1.svg").arg(name));
    QByteArray data;
    if (f.open(QIODevice::ReadOnly)) data = f.readAll();
    g_svg.insert(name, data);
    return data;
}

QPixmap render(const QString& name, QSize size, qreal dpr, const QColor& color) {
    const QString key = QStringLiteral("%1|%2x%3|%4|%5").arg(name).arg(size.width()).arg(size.height()).arg(dpr).arg(
        QString::number(color.rgba(), 16));
    {
        QMutexLocker lock(&g_mutex);
        auto it = g_pixmaps.find(key);
        if (it != g_pixmaps.end()) return *it;
    }
    QByteArray svg = loadSvg(name);
    if (svg.isEmpty()) svg = loadSvg(QStringLiteral("component"));
    svg.replace("currentColor", color.name(QColor::HexRgb).toLatin1());
    QPixmap pm(size * dpr);
    pm.fill(Qt::transparent);
    {
        QSvgRenderer renderer(svg);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        if (color.alpha() < 255) p.setOpacity(color.alphaF());
        renderer.render(&p, QRectF(QPointF(0, 0), QSizeF(size) * dpr));
    }
    pm.setDevicePixelRatio(dpr);
    QMutexLocker lock(&g_mutex);
    g_pixmaps.insert(key, pm);
    return pm;
}

class TintIconEngine final : public QIconEngine {
public:
    TintIconEngine(QString name, Icons::Tint tint, QColor fixed) : m_name(std::move(name)), m_tint(tint), m_fixed(fixed) {}

    QColor colorFor(QIcon::Mode mode, QIcon::State state) const {
        const ThemePalette& c = colors();
        if (m_fixed.isValid()) return mode == QIcon::Disabled ? withAlpha(m_fixed, 90) : m_fixed;
        if (mode == QIcon::Disabled) return c.textFaint;
        if (m_tint != Icons::Tint::Theme) return Icons::tintColor(m_tint);
        if (mode == QIcon::Selected) return c.onAccent;
        if (state == QIcon::On) return c.text;
        if (mode == QIcon::Active) return c.text;
        return c.textDim;
    }

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override {
        const qreal dpr = painter->device() ? painter->device()->devicePixelRatioF() : qApp->devicePixelRatio();
        painter->drawPixmap(rect, render(m_name, rect.size(), dpr, colorFor(mode, state)));
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        return render(m_name, size, 1.0, colorFor(mode, state));
    }
    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
        return render(m_name, size, scale, colorFor(mode, state));
    }
    QIconEngine* clone() const override { return new TintIconEngine(m_name, m_tint, m_fixed); }
    QString key() const override { return QStringLiteral("OxTintIcon"); }
    QString iconName() override { return m_name; }
    bool isNull() override { return false; }

private:
    QString m_name;
    Icons::Tint m_tint;
    QColor m_fixed;
};

} // namespace

QColor Icons::tintColor(Tint tint) {
    const ThemePalette& c = colors();
    switch (tint) {
    case Tint::Accent: return c.accentText;
    case Tint::Text: return c.text;
    case Tint::Dim: return c.textDim;
    case Tint::Faint: return c.textFaint;
    case Tint::Success: return c.success;
    case Tint::Warning: return c.warning;
    case Tint::Error: return c.error;
    case Tint::Info: return c.info;
    case Tint::OnAccent: return c.onAccent;
    case Tint::Theme: break;
    }
    return c.textDim;
}

QIcon Icons::get(const QString& name, Tint tint) { return QIcon(new TintIconEngine(name, tint, {})); }

QIcon Icons::fixed(const QString& name, const QColor& color) { return QIcon(new TintIconEngine(name, Tint::Theme, color)); }

QPixmap Icons::pixmap(const QString& name, int sizePx, const QColor& color) {
    return render(name, QSize(sizePx, sizePx), 1.0, color);
}

bool Icons::exists(const QString& name) { return !loadSvg(name).isEmpty(); }

QStringList Icons::names() {
    QStringList out;
    for (const QString& f : QDir(QStringLiteral(":/icons")).entryList({QStringLiteral("*.svg")})) {
        out << f.chopped(4);
    }
    return out;
}

QByteArray Icons::svgData(const QString& name) { return loadSvg(name); }

void Icons::clearCache() {
    QMutexLocker lock(&g_mutex);
    g_pixmaps.clear();
}

QString Icons::forComponent(const QString& iconHint, const QString& componentName) {
    if (!iconHint.isEmpty() && exists(iconHint)) return iconHint;
    static const QHash<QString, QString> byName = {
        {"Transform", "transform"},     {"Camera", "camera"},         {"Light", "light"},
        {"MeshRenderer", "mesh"},       {"Environment", "environment"}, {"Tags", "tag"},
        {"SaveGame", "save-game"},      {"PrefabInstance", "prefab"}, {"RigidBody", "physics"},
        {"Collider", "collision"},      {"AudioSource", "audio"},     {"AudioListener", "audio"},
        {"Script", "script"},           {"Animator", "animation"},    {"NavAgent", "map"},
        {"Spline", "bezier-curve"},     {"CharacterController", "person-walking"},
        {"PostProcessVolume", "postprocess"}, {"FogVolume", "cloud"},  {"VolumetricFog", "cloud"},
        {"CloudLayer", "cloud"},        {"VegetationPrototypes", "tree"}, {"TerrainRender", "mountain"},
        {"ParticleEmitter", "particles"}, {"WaterSurface", "water"},  {"ReflectionProbe", "globe"},
    };
    if (auto it = byName.find(componentName); it != byName.end()) return *it;
    static const QHash<QString, QString> byHint = {{"cube", "cube"}, {"sound", "audio"}, {"physics", "physics"}};
    if (auto it = byHint.find(iconHint); it != byHint.end()) return *it;
    return QStringLiteral("component");
}

QString Icons::forCategory(const QString& category) {
    static const QHash<QString, QString> map = {
        {"Rendering", "rendering"}, {"Effects", "sparkles"}, {"Motion", "speed"}, {"Physics", "physics"},  {"Animation", "animation"}, {"Splines", "bezier-curve"},
        {"Audio", "audio"},         {"AI", "behavior-tree"}, {"Scripting", "script"},    {"Networking", "network"},
        {"World", "mountain"},      {"Core", "entity"},      {"Gameplay", "gamepad"},    {"Lighting", "light"},
    };
    if (auto it = map.find(category); it != map.end()) return *it;
    return QStringLiteral("component");
}

QString Icons::forAssetType(const QString& type) {
    static const QHash<QString, QString> map = {
        {"Folder", "folder"},   {"Scene", "scene"},     {"Prefab", "prefab"},     {"Mesh", "mesh"},
        {"Model", "model"},     {"Material", "material"}, {"Texture", "texture"}, {"Audio", "audio"},
        {"Script", "script"},   {"Shader", "shader"},   {"Font", "font"},         {"Animation", "animation"},
        {"Physics", "physics"}, {"Data", "file"},       {"SaveGame", "save-game"}, {"BehaviorTree", "behavior-tree"},
        {"AnimatorController", "animation"}, {"Skeleton", "person-running"}, {"NavMesh", "navmesh"}, {"Heightmap", "mountain"},
    };
    if (auto it = map.find(type); it != map.end()) return *it;
    return QStringLiteral("file");
}

} // namespace ox::editor
