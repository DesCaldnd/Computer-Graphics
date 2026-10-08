#pragma once

#include <QString>
#include <QVariant>

namespace ox::editor {

class EditorContext;

// Pushes edited project settings into the running engine without a restart: input mappings (InputSystem) and the
// physics gravity. Module toggles, asset dirs and the fixed rate apply when the project is reopened.
void applyProjectSettingsLive(EditorContext& ctx);

// Runtime UserSettings of the project (user://settings.json — what the shipped game reads/writes from its options
// menu): graphics, audio, mouse, language. Keys: "graphics.resolutionX", "graphics.resolutionY",
// "graphics.windowMode" (0..2), "graphics.vsync", "graphics.maxFps", "graphics.quality" (Low..Ultra/Custom),
// "graphics.fov", "graphics.rayTracing", "graphics.upscaler" (Off/FSR1/DLSS), "audio.masterVolume",
// "mouseSensitivity", "invertY", "language".
[[nodiscard]] bool userSettingsAvailable(EditorContext& ctx);
[[nodiscard]] QString userSettingsFile(EditorContext& ctx);
[[nodiscard]] QVariant userSetting(EditorContext& ctx, const QString& key);
// Sets the value and writes user://settings.json.
void setUserSetting(EditorContext& ctx, const QString& key, const QVariant& value);
bool reloadUserSettings(EditorContext& ctx);
// Settings::apply(): user graphics/audio into the cvars/scalability of the editor session (preview).
void applyUserSettingsToSession(EditorContext& ctx);

} // namespace ox::editor
