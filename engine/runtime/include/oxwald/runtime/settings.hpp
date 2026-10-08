#pragma once

#include <oxwald/core/cvar.hpp>
#include <oxwald/core/events.hpp>
#include <oxwald/runtime/project.hpp>

#include <glm/vec2.hpp>

#include <map>
#include <string>

// Per-user settings (user://settings.json) layered over project defaults, applied through cvars and
// scalability so console changes, the options menu and config files all go through the same path:
//
//   project defaults (ProjectSettings) -> user settings (UserSettings) -> command line (--quality, --cvar)
//
// Runtime owns these cvars: r.VSync, r.WindowMode, r.ResolutionX/Y, r.Monitor, t.MaxFPS, g.FOV, a.MasterVolume.
// Renderer-owned cvars (r.RayTracing, r.Upscaler, r.Upscaler.Quality) are set by name; values stay pending in
// the CVarRegistry until the render module registers them.
namespace ox {

class Vfs;

enum class WindowMode : u8 { Windowed, Borderless, Fullscreen };

struct GraphicsSettings {
    glm::ivec2 resolution{0, 0}; // 0 = default window size / desktop resolution
    WindowMode windowMode = WindowMode::Windowed;
    i32 monitor = 0;
    bool vsync = true;
    i32 maxFps = 0;                          // 0 = unlimited (or vsync)
    std::string quality = "High";            // overall: Low/Medium/High/Ultra, "Custom" = only `groups`
    std::map<std::string, std::string> groups; // per-group overrides ("Shadows": "Low")
    bool rayTracing = false;
    std::string upscaler = "Off";            // Off / FSR1 / DLSS
    std::string upscalerQuality = "Quality"; // UltraPerformance/Performance/Balanced/Quality/DLAA
    f32 fov = 90.0f;                         // horizontal-ish gameplay FOV in degrees (g.FOV)
};

struct AudioSettings {
    f32 masterVolume = 1.0f;
    std::map<std::string, f32> busVolumes; // "Music", "SFX", "Voice", "UI", "Ambience"
};

struct UserSettings {
    GraphicsSettings graphics;
    AudioSettings audio;
    std::map<std::string, std::string> inputRebinds; // InputSystem::rebinds()
    f32 mouseSensitivity = 1.0f;
    bool invertY = false;
    std::string language = "en";
    std::map<std::string, std::string> cvars; // other persisted cvar overrides
};

enum class SettingsCategory : u32 { None = 0, Graphics = 1, Audio = 2, Input = 4, Gameplay = 8, All = 15 };
[[nodiscard]] constexpr SettingsCategory operator|(SettingsCategory a, SettingsCategory b) {
    return SettingsCategory(u32(a) | u32(b));
}
[[nodiscard]] constexpr bool hasCategory(SettingsCategory set, SettingsCategory c) { return (u32(set) & u32(c)) != 0; }

class Settings {
public:
    static constexpr std::string_view kDefaultUri = "user://settings.json";

    // vfs may be null (then loadFrom/saveTo with native paths only).
    explicit Settings(Vfs* vfs = nullptr, std::string uri = std::string(kDefaultUri));

    [[nodiscard]] const ProjectSettings& project() const { return m_project; }
    void setProject(const ProjectSettings& project) { m_project = project; }
    // Project defaults -> cvars/scalability (call before applying user settings).
    void applyProjectDefaults();

    [[nodiscard]] UserSettings& user() { return m_user; }
    [[nodiscard]] const UserSettings& user() const { return m_user; }

    // Missing file = defaults (not an error).
    Status load();
    Status save();
    Status loadFrom(const std::filesystem::path& path);
    Status saveTo(const std::filesystem::path& path);
    [[nodiscard]] nlohmann::ordered_json toJson();
    bool fromJson(const nlohmann::ordered_json& json);

    // Pushes user settings into cvars/scalability and emits `changed`.
    void apply(SettingsCategory categories = SettingsCategory::All);
    void setGraphics(const GraphicsSettings& graphics);
    void setAudio(const AudioSettings& audio);
    void setInputRebinds(const std::map<std::string, std::string>& rebinds);
    // Reads the current cvar/scalability state back into user() (e.g. after console edits) so save() keeps it.
    void captureFromCVars();

    // Bus volume (without master) from user settings, falling back to the project default, else 1.
    [[nodiscard]] f32 busVolume(std::string_view bus) const;

    // Emitted after apply()/setX() on the calling thread.
    Signal<SettingsCategory> changed;

private:
    Vfs* m_vfs;
    std::string m_uri;
    ProjectSettings m_project;
    UserSettings m_user;
};

void registerSettingsTypes();

// Runtime-owned cvars (defined in settings.cpp).
namespace cvars {
CVar<bool>& vsync();
CVar<int>& windowMode();
CVar<int>& resolutionX();
CVar<int>& resolutionY();
CVar<int>& monitor();
CVar<int>& maxFps();
CVar<float>& fov();
CVar<float>& masterVolume();
} // namespace cvars

} // namespace ox
