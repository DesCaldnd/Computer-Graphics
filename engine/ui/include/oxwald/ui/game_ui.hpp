#pragma once

// GameUI: RmlUi game UI. One Rml::Context ("game") with documents loaded from the VFS (default root
// project://UI/), hot reload of .rml/.rcss, data models (C++ and Lua), event listeners, localisation hook and the
// engine settings menu model. Rendered into a UiFrame (drawn by UiOverlayFeature at output resolution).
//
//   GameUI ui(bridge.textures(), &vfs);
//   ui.load("main_menu.rml", /*show*/ true);              // project://UI/main_menu.rml
//   ui.addEventListener("main_menu.rml", "play", "click", [](Rml::Event&) { ... });
//   per frame: ui.update(dt, fbSize, dpi);  ui.render(frame);
//
// RmlUi has process-wide state (system/file interfaces, font engine): only one GameUI may exist at a time.

#include <oxwald/core/events.hpp>
#include <oxwald/runtime/input.hpp>
#include <oxwald/ui/draw_data.hpp>

#include <RmlUi/Core/DataModelHandle.h>

#include <glm/vec2.hpp>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Rml {
class Context;
class ElementDocument;
class Element;
class Event;
} // namespace Rml

namespace ox {
class Vfs;
class Settings;
} // namespace ox

namespace ox::ui {

namespace detail {
class RmlRenderer;
class RmlSystem;
class RmlFiles;
} // namespace detail

struct GameUIConfig {
    std::string root = "project://UI/"; // relative document paths are resolved against this
    bool hotReload = true;
    f64 hotReloadInterval = 0.5;         // seconds between mtime polls
    bool loadDefaultFonts = true;        // Inter (+ bold) and JetBrains Mono from the ui resources
    std::string contextName = "game";
};

// Hooks for the settings menu model (UiSystem fills them).
struct SettingsMenuHooks {
    // "Auto" quality: start the GPU benchmark; the result arrives later via onAutoDetectResult().
    std::function<void()> requestAutoDetect;
    // Ray tracing / upscaler availability (render caps). Default: RT unavailable, FSR1 only.
    std::function<bool(std::string& reason)> rayTracingAvailable;
    std::function<std::vector<UpscalerAvailability>()> upscalers;
    // Resolution list ("1920x1080"); default: common 16:9 modes.
    std::function<std::vector<glm::ivec2>()> resolutions;
    // Save after every change (user://settings.json).
    bool saveOnChange = false;
};

class GameUI {
public:
    using EventFn = std::function<void(Rml::Event&)>;
    using Translator = std::function<std::optional<std::string>(std::string_view text)>;

    GameUI(UiTextureStore& textures, Vfs* vfs, GameUIConfig config = {});
    ~GameUI();
    GameUI(const GameUI&) = delete;
    GameUI& operator=(const GameUI&) = delete;

    [[nodiscard]] Rml::Context* context() const { return m_context; }
    [[nodiscard]] const GameUIConfig& config() const { return m_config; }

    // ---- documents (name = the path given to load()) ----
    Rml::ElementDocument* load(std::string_view path, bool show = false);
    [[nodiscard]] Rml::ElementDocument* document(std::string_view name) const;
    bool show(std::string_view name, bool modal = false);
    bool hide(std::string_view name);
    bool close(std::string_view name);
    [[nodiscard]] bool isVisible(std::string_view name) const;
    [[nodiscard]] std::vector<std::string> documents() const;
    [[nodiscard]] std::string resolve(std::string_view path) const; // root + path for relative paths

    // Listener on an element (by id) of a document; re-attached after hot reloads. Returns an id.
    u64 addEventListener(std::string_view document, std::string_view elementId, std::string_view event, EventFn fn);
    void removeEventListener(u64 id);

    // ---- frame (game thread) ----
    void update(f64 dt, glm::uvec2 framebufferSize, f32 dpiScale = 1.0f);
    void render(UiFrame& out);

    // ---- input (positions in framebuffer pixels); true = consumed ----
    bool processEvent(const InputEvent& event);
    [[nodiscard]] bool wantsMouse() const;
    [[nodiscard]] bool hasTextFocus() const;

    // ---- hot reload ----
    // Re-reads changed .rml/.rcss below the directories of loaded documents. Returns reloaded documents.
    usize pollHotReload(bool force = false);
    Signal<const std::string&> documentReloaded;

    // ---- data models ----
    Rml::DataModelConstructor createModel(std::string_view name);
    [[nodiscard]] Rml::DataModelHandle model(std::string_view name) const;
    bool removeModel(std::string_view name);

    // ---- localisation ----
    // Called for every text node and attribute; return a replacement (or nullopt). setTranslations() installs a
    // table translator for "#key" texts. Changing either reloads the documents.
    void setTranslator(Translator translator);
    void setTranslations(std::map<std::string, std::string> table);

    // ---- engine settings menu (data model "settings", used by resources/sample/settings.rml) ----
    bool bindSettingsMenu(Settings& settings, SettingsMenuHooks hooks = {});
    // Applies a finished quality benchmark to the menu state (UiSystem calls it).
    void onAutoDetectResult(const std::string& summary);
    // Re-reads the menu state from Settings (e.g. after console edits).
    void refreshSettingsMenu();

    void setDebuggerVisible(bool visible);
    [[nodiscard]] u32 errorCount() const;

private:
    struct Doc {
        std::string name;
        std::string uri;
        Rml::ElementDocument* document = nullptr;
        bool visible = false;
        bool modal = false;
    };
    struct Listener;
    struct SettingsModel;

    Doc* findDoc(std::string_view name);
    const Doc* findDoc(std::string_view name) const;
    void attachListeners(Doc& doc);
    void reloadAll();
    void snapshotMtimes();
    int modifiers() const;

    UiTextureStore& m_textures;
    Vfs* m_vfs;
    GameUIConfig m_config;
    std::unique_ptr<detail::RmlRenderer> m_renderer;
    std::unique_ptr<detail::RmlSystem> m_system;
    std::unique_ptr<detail::RmlFiles> m_files;
    Rml::Context* m_context = nullptr;
    std::vector<Doc> m_docs;
    std::vector<std::unique_ptr<Listener>> m_listeners;
    u64 m_nextListener = 1;
    std::map<std::string, u64, std::less<>> m_mtimes;
    f64 m_pollTimer = 0.0;
    glm::uvec2 m_size{0, 0};
    f32 m_dpi = 1.0f;
    std::map<std::string, std::string> m_translations;
    std::unique_ptr<SettingsModel> m_settings;
    bool m_ctrl = false, m_shift = false, m_alt = false, m_meta = false;
    bool m_debugger = false;
};

} // namespace ox::ui
