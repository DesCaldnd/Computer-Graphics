#include "integration/runtime_settings.hpp"

#include "core/editor_context.hpp"

#include <oxwald/core/log.hpp>

#if OX_EDITOR_HAS_RUNTIME
#include <oxwald/core/vfs.hpp>
#include <oxwald/runtime/engine.hpp>
#include <oxwald/runtime/settings.hpp>
#endif
#if OX_EDITOR_HAS_PHYSICS
#include <oxwald/physics/physics_world.hpp>
#endif

namespace ox::editor {

#if OX_EDITOR_HAS_RUNTIME

void applyProjectSettingsLive(EditorContext& ctx) {
    Engine* e = ctx.engine();
    Project* p = ctx.project();
    if (!e || !p) return;
    const ProjectSettings ps = p->runtimeSettings();
    e->input().setMappings(ps.input);
    e->input().applyRebinds(e->settings().user().inputRebinds);
#if OX_EDITOR_HAS_PHYSICS
    if (auto* phys = e->services().tryGet<physics::PhysicsWorld>()) phys->setGravity(ps.physics.gravity);
#endif
    e->settings().setProject(ps);
}

bool userSettingsAvailable(EditorContext& ctx) { return ctx.engine() != nullptr; }

QString userSettingsFile(EditorContext& ctx) {
    Engine* e = ctx.engine();
    if (!e) return {};
    if (auto native = e->vfs().resolveNative(Settings::kDefaultUri)) return QString::fromStdString(native->string());
    return QString::fromLatin1(Settings::kDefaultUri.data(), qsizetype(Settings::kDefaultUri.size()));
}

QVariant userSetting(EditorContext& ctx, const QString& key) {
    Engine* e = ctx.engine();
    if (!e) return {};
    const UserSettings& u = e->settings().user();
    const GraphicsSettings& g = u.graphics;
    if (key == QLatin1String("graphics.resolutionX")) return g.resolution.x;
    if (key == QLatin1String("graphics.resolutionY")) return g.resolution.y;
    if (key == QLatin1String("graphics.windowMode")) return int(g.windowMode);
    if (key == QLatin1String("graphics.vsync")) return g.vsync;
    if (key == QLatin1String("graphics.maxFps")) return g.maxFps;
    if (key == QLatin1String("graphics.quality")) return QString::fromStdString(g.quality);
    if (key == QLatin1String("graphics.fov")) return double(g.fov);
    if (key == QLatin1String("graphics.rayTracing")) return g.rayTracing;
    if (key == QLatin1String("graphics.upscaler")) return QString::fromStdString(g.upscaler);
    if (key == QLatin1String("audio.masterVolume")) return double(u.audio.masterVolume);
    if (key == QLatin1String("mouseSensitivity")) return double(u.mouseSensitivity);
    if (key == QLatin1String("invertY")) return u.invertY;
    if (key == QLatin1String("language")) return QString::fromStdString(u.language);
    return {};
}

void setUserSetting(EditorContext& ctx, const QString& key, const QVariant& v) {
    Engine* e = ctx.engine();
    if (!e) return;
    UserSettings& u = e->settings().user();
    GraphicsSettings& g = u.graphics;
    if (key == QLatin1String("graphics.resolutionX")) g.resolution.x = v.toInt();
    else if (key == QLatin1String("graphics.resolutionY")) g.resolution.y = v.toInt();
    else if (key == QLatin1String("graphics.windowMode")) g.windowMode = WindowMode(std::clamp(v.toInt(), 0, 2));
    else if (key == QLatin1String("graphics.vsync")) g.vsync = v.toBool();
    else if (key == QLatin1String("graphics.maxFps")) g.maxFps = v.toInt();
    else if (key == QLatin1String("graphics.quality")) g.quality = v.toString().toStdString();
    else if (key == QLatin1String("graphics.fov")) g.fov = float(v.toDouble());
    else if (key == QLatin1String("graphics.rayTracing")) g.rayTracing = v.toBool();
    else if (key == QLatin1String("graphics.upscaler")) g.upscaler = v.toString().toStdString();
    else if (key == QLatin1String("audio.masterVolume")) u.audio.masterVolume = float(v.toDouble());
    else if (key == QLatin1String("mouseSensitivity")) u.mouseSensitivity = float(v.toDouble());
    else if (key == QLatin1String("invertY")) u.invertY = v.toBool();
    else if (key == QLatin1String("language")) u.language = v.toString().toStdString();
    else return;
    if (auto st = e->settings().save(); !st) OX_LOG_WARN("editor", "user settings: {}", st.error().message);
}

bool reloadUserSettings(EditorContext& ctx) {
    Engine* e = ctx.engine();
    return e && e->settings().load().hasValue();
}

void applyUserSettingsToSession(EditorContext& ctx) {
    if (Engine* e = ctx.engine()) e->settings().apply();
}

#else

void applyProjectSettingsLive(EditorContext&) {}
bool userSettingsAvailable(EditorContext&) { return false; }
QString userSettingsFile(EditorContext&) { return {}; }
QVariant userSetting(EditorContext&, const QString&) { return {}; }
void setUserSetting(EditorContext&, const QString&, const QVariant&) {}
bool reloadUserSettings(EditorContext&) { return false; }
void applyUserSettingsToSession(EditorContext&) {}

#endif

} // namespace ox::editor
