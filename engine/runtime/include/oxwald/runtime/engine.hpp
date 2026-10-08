#pragma once

#include <oxwald/core/events.hpp>
#include <oxwald/core/result.hpp>
#include <oxwald/core/services.hpp>
#include <oxwald/core/time.hpp>
#include <oxwald/runtime/console.hpp>
#include <oxwald/runtime/input.hpp>
#include <oxwald/runtime/project.hpp>
#include <oxwald/runtime/render_pipeline.hpp>
#include <oxwald/runtime/renderer.hpp>
#include <oxwald/runtime/save_game.hpp>
#include <oxwald/runtime/settings.hpp>
#include <oxwald/scene/system.hpp>
#include <oxwald/scene/world.hpp>

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ox {

class Engine;
class IPlatform;
class JobSystem;
class Vfs;

enum class EngineMode : u8 { Edit, Play };

struct EngineConfig {
    std::string appName = "Oxwald";
    bool editor = false;         // starts in Edit mode; play mode simulates a clone of the edited world
    bool headless = false;       // no window: NullRenderer unless a renderer was set, offline audio
    bool dedicatedServer = false; // headless + no audio/rendering work
    bool threadedRendering = true; // game thread + render thread (false: extract+render inline, editor/debug)
    u32 workerThreads = 0;       // job system threads incl. main (0 = hardware)
    std::optional<f64> fixedRate; // Hz; default from the project (60)
    std::optional<u32> maxFixedSteps; // default from the project (8)
    f64 targetFps = 0.0;         // frame limiter (0 = t.MaxFPS cvar / unlimited)
    f64 maxFrameDelta = 0.25;    // clamps hitches (debugger breaks) before time scaling
    std::filesystem::path projectPath; // .oxproj or its directory; empty = no project
    std::optional<ProjectSettings> projectSettings; // in-memory project (tests/tools) when no projectPath
    // Cooked game (assets module): assets and project:// are served from this .oxpak only (PakAssetSource, no
    // AssetRegistry/importers). Without projectPath the project settings come from the pak's <Name>.oxproj.
    std::filesystem::path pakPath;
    std::vector<std::filesystem::path> patchPaks; // mounted after pakPath; later paks override earlier ones
    usize assetMemoryBudget = 0;                  // AssetManager budget in bytes (0 = unlimited)
    std::filesystem::path userDir;   // user:// root; default paths::userDataDir(appName or project name)
    std::filesystem::path engineDir; // engine:// root; default paths::engineSourceDir()
    std::string startupScene;    // overrides the project's startup scene ("" = project's, "-" = none)
    bool loadUserSettings = true;
    bool saveUserSettingsOnShutdown = true;
    bool fileWatching = false;   // poll the FileWatcher each frame (editor/hot reload)
    std::optional<QualityLevel> quality; // command line --quality (after user settings)
    std::vector<std::string> cvars;      // "name=value" / "name value" applied last
    RenderSurface surface;       // when no platform is attached
};

struct EngineStats {
    u64 frame = 0;
    f64 frameMs = 0.0;        // wall time of the whole frame (incl. pacing sleep)
    f64 gameMs = 0.0;         // game thread work (input, simulation, extract)
    f64 simulationMs = 0.0;   // SystemScheduler::tick
    f64 extractMs = 0.0;
    f64 renderMs = 0.0;       // render thread time of the last rendered frame
    f64 waitForRenderMs = 0.0; // game thread blocked on the render thread
    f64 sleepMs = 0.0;        // frame pacing
    u32 fixedSteps = 0;       // fixed steps this frame
    u64 totalFixedSteps = 0;
    f64 alpha = 0.0;          // interpolation factor
    f64 droppedTime = 0.0;    // simulation time dropped by the max-substeps guard (total)
    f64 fps = 0.0;            // smoothed
    f64 gameTime = 0.0;       // scaled
    f64 realTime = 0.0;
};

// Per-frame timing handed to IEngineModule::preUpdate.
struct FrameTime {
    f64 realDt = 0.0;   // unscaled, clamped frame delta
    f64 dt = 0.0;       // game delta (scaled; 0 while paused unless stepping)
    u64 frameIndex = 0;
    f64 timeScale = 1.0;
    bool paused = false;
    bool stepping = false; // paused frame advanced by stepFrames()
    bool playing = false;  // play mode and not loading
};

// Engine extension point: modules (physics, audio, script, gameplay, render integrations, user code) create
// their services and systems through this interface. init() runs in registration order after the core services
// exist; shutdown() in reverse order before the services are destroyed (also in reverse order).
class IEngineModule {
public:
    virtual ~IEngineModule() = default;
    [[nodiscard]] virtual std::string_view name() const = 0;
    virtual void registerTypes() {}
    virtual Status init(Engine& engine, Services& services) { return {}; }
    virtual void registerSystems(Engine& engine, SystemScheduler& scheduler) {}
    // Game thread, every frame after input and before the SystemScheduler tick (= start of PreUpdate), in module
    // registration order.
    virtual void preUpdate(Engine& engine, const FrameTime& time) {}
    // The active world is about to be destroyed or replaced (level change, leaving play mode, shutdown).
    virtual void onWorldUnloading(Engine& engine, World& world) {}
    // The active world was replaced (level change, play mode).
    virtual void onWorldChanged(Engine& engine, World& world) {}
    virtual void shutdown(Engine& engine, Services& services) {}
};

struct LoadingScreenHooks {
    std::function<void(const std::string& level)> begin;
    std::function<void(const std::string& level, bool success)> end;
};

class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // ---- setup (before init) ----
    void addModule(std::unique_ptr<IEngineModule> module);
    // Takes ownership; default NullRenderer.
    void setRenderer(std::unique_ptr<IRenderer> renderer);
    // Not owned; pumps events, provides the surface and window settings. May be set before init only.
    void setPlatform(IPlatform* platform);

    Status init(const EngineConfig& config);
    // Graceful shutdown: render thread, pending saves, modules (reverse), services (reverse). Idempotent.
    void shutdown();
    [[nodiscard]] bool initialized() const { return m_initialized; }

    // ---- loop (game thread) ----
    // One frame with measured wall time. Returns false once quit was requested / the window closed.
    bool tick();
    // One frame with an explicit unscaled delta (editor, tests, lockstep servers). No pacing sleep.
    bool tick(f64 realDt);
    // Runs frames until quit (maxFrames > 0 limits the count).
    void run(u64 maxFrames = 0);
    void requestQuit() { m_quit.store(true); }
    [[nodiscard]] bool quitRequested() const { return m_quit.load(); }

    // ---- modes and time ----
    [[nodiscard]] EngineMode mode() const { return m_mode; }
    // Editor: clones the edit world and simulates the copy. No-op outside editor configs.
    void enterPlayMode();
    void exitPlayMode();
    void setPaused(bool paused) { m_paused = paused; }
    [[nodiscard]] bool paused() const { return m_paused; }
    void setTimeScale(f64 scale) { m_timeScale = scale < 0.0 ? 0.0 : scale; }
    [[nodiscard]] f64 timeScale() const { return m_timeScale; }
    // While paused: advance `frames` frames of exactly one fixed step each.
    void stepFrames(u32 frames = 1) { m_stepRequests += frames; }

    // ---- worlds and levels ----
    [[nodiscard]] World& world();           // active world (play copy in editor play mode)
    [[nodiscard]] World& editWorld() { return *m_world; }
    // Replaces the level world synchronously ("project://levels/a.oxscene" or a native path).
    Status loadScene(std::string_view uriOrPath);
    // Adds the scene's entities to the active world; unloadAdditive destroys them again.
    Result<std::vector<Entity>> loadSceneAdditive(std::string_view uriOrPath);
    Status unloadAdditive(std::string_view uriOrPath);
    [[nodiscard]] std::vector<std::string> additiveScenes() const;
    // Asynchronous level switch: file read/decoding on the job system, loading screen hooks, autosave on level
    // change; the swap happens at the start of a later frame.
    void requestLevelChange(std::string uriOrPath);
    [[nodiscard]] bool loading() const { return m_loadState != nullptr; }
    void setLoadingScreenHooks(LoadingScreenHooks hooks) { m_loadingHooks = std::move(hooks); }
    // Installs an externally built world (editor new scene, tests).
    void setWorld(std::unique_ptr<World> world, std::string levelUri = {});
    [[nodiscard]] const std::string& currentLevel() const { return m_level; }

    // ---- save games (game thread) ----
    Result<SaveResult> saveGame(std::string_view slot, std::string displayName = {});
    Result<LoadResult> loadGame(std::string_view slot);

    // ---- access ----
    [[nodiscard]] const EngineConfig& config() const { return m_config; }
    [[nodiscard]] Services& services() { return m_services; }
    [[nodiscard]] SystemScheduler& scheduler() { return *m_scheduler; }
    [[nodiscard]] InputSystem& input() { return *m_input; }
    [[nodiscard]] Settings& settings() { return *m_settings; }
    [[nodiscard]] SaveGameSystem& saves() { return *m_saves; }
    [[nodiscard]] Console& console() { return *m_console; }
    [[nodiscard]] IRenderer& renderer() { return *m_renderer; }
    [[nodiscard]] RenderPipeline& pipeline() { return m_pipeline; }
    [[nodiscard]] JobSystem& jobs() { return *m_jobs; }
    [[nodiscard]] Vfs& vfs() { return *m_vfs; }
    [[nodiscard]] const ProjectSettings& projectSettings() const { return m_projectSettings; }
    [[nodiscard]] const std::optional<Project>& project() const { return m_project; }
    [[nodiscard]] const EngineStats& stats() const { return m_stats; }
    // Timing of the frame being simulated (valid during tick()).
    [[nodiscard]] const FrameTime& frameTime() const { return m_frameTime; }
    [[nodiscard]] IPlatform* platform() const { return m_platform; }
    [[nodiscard]] IEngineModule* findModule(std::string_view name) const;

    // Applies graphics settings to the platform (window) and renderer (on its thread). Called automatically when
    // Settings::changed(Graphics) fires or a r.* cvar changes from the console.
    void applyGraphicsSettings();

    Signal<const std::string&> levelLoaded;    // after the new world is active
    Signal<const std::string&> levelUnloading; // before the old world is destroyed
    Signal<EngineMode> modeChanged;
    Signal<const EngineStats&> frameEnded;

private:
    struct LoadState;
    struct AdditiveScene {
        std::string uri;
        std::vector<Uuid> roots;
    };

    Status createServices();
    Status loadProject();
    void mountFileSystems();
    void applyCommandLineOverrides();
    void attachWorld();
    void detachWorld();
    void installWorld(std::unique_ptr<World> world, const std::string& level);
    Result<serial::Document> readSceneDocument(std::string_view uriOrPath) const;
    Result<std::unique_ptr<World>> buildWorld(const serial::Document& doc) const;
    void updateLevelLoading();
    void beginFrame();
    bool frame(f64 realDt, bool pace);
    void pace(Clock::TimePoint frameStart);
    [[nodiscard]] f64 targetFps() const;

    EngineConfig m_config;
    bool m_initialized = false;
    std::atomic<bool> m_quit{false};

    Services m_services;
    std::vector<std::unique_ptr<IEngineModule>> m_modules;
    std::vector<IEngineModule*> m_initializedModules;
    std::unique_ptr<IRenderer> m_pendingRenderer;
    IRenderer* m_renderer = nullptr;
    bool m_rendererInitialized = false;
    IPlatform* m_platform = nullptr;
    RenderPipeline m_pipeline;

    JobSystem* m_jobs = nullptr;
    Vfs* m_vfs = nullptr;
    InputSystem* m_input = nullptr;
    Settings* m_settings = nullptr;
    SaveGameSystem* m_saves = nullptr;
    Console* m_console = nullptr;

    std::optional<Project> m_project;
    ProjectSettings m_projectSettings;

    std::unique_ptr<SystemScheduler> m_scheduler;
    std::unique_ptr<World> m_world;     // level / edit world
    std::unique_ptr<World> m_playWorld; // editor play-mode copy
    World* m_attachedWorld = nullptr;
    std::string m_level;
    std::vector<AdditiveScene> m_additive;
    std::shared_ptr<LoadState> m_loadState;
    std::string m_pendingLevel;
    LoadingScreenHooks m_loadingHooks;

    EngineMode m_mode = EngineMode::Play;
    bool m_paused = false;
    f64 m_timeScale = 1.0;
    u32 m_stepRequests = 0;

    Clock::TimePoint m_lastTick{};
    bool m_hasLastTick = false;
    Clock::TimePoint m_startTime{};
    FrameTimer m_frameTimer;
    EngineStats m_stats;
    FrameTime m_frameTime;
    u64 m_frameIndex = 0;
    f64 m_gameTime = 0.0;
    glm::uvec2 m_lastFramebuffer{0, 0};

    std::vector<ScopedConnection> m_connections;
};

// Built-in engine modules for the optional engine libraries that are linked into this build (physics, audio,
// script, ...). Engine::init adds them automatically unless disabled in the project's module toggles.
std::vector<std::unique_ptr<IEngineModule>> makeBuiltinModules(const EngineConfig& config,
                                                               const ProjectSettings& project);

} // namespace ox
