#pragma once

// Native game module of OxwaldShowcase ("Showcase", enabled by the project's "modules": {"Showcase": true}).
//
// Everything the Lua game code cannot do through the stock engine APIs lives here:
//   * Lua table `showcase` (showcase_lua.cpp): launch mode/args, cvars and console, levels, save games (deferred to
//     the start of the next frame so a script never destroys the world it is running in), render/RTX/upscaler
//     availability and GPU timings, audio buses (reverb zone, ducking), the net demo, screenshots;
//   * the guided tour and photo mode (tour.cpp): C++ coroutines on the engine's CoroutineScheduler fly a camera
//     through every station, show captions and take screenshots;
//   * the listen-server demo (net_demo.cpp): the station world is the server, an in-process "bot" client with its
//     own World receives it over a lossy in-memory transport and its interpolated view is mirrored as ghosts.

#include <oxwald/core/math.hpp>
#include <oxwald/runtime/engine.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ox::script {
class ScriptVM;
}

namespace ox::showcase {

enum class LaunchMode : u8 { Play, Tour, Photo };

struct StationInfo {
    std::string id;     // "lighting"
    std::string title;  // Russian title
    std::string scene;  // project:// URI
    std::string guide;  // guide chapter shown as "Подробнее"
};

struct LaunchArgs {
    LaunchMode mode = LaunchMode::Play;
    std::filesystem::path shotsDir; // --shots <dir>: tour screenshots (<dir>/<station id>.png)
    bool quitAfterTour = false;     // --quit (default when headless)
    std::string onlyStation;        // --station <id>: tour only this station
    std::vector<std::string> raw;
};

LaunchArgs parseLaunchArgs(const std::vector<std::string>& args, bool headless);

class NetDemo;
class Tour;

class ShowcaseModule final : public IEngineModule {
public:
    ShowcaseModule();
    ~ShowcaseModule() override;

    [[nodiscard]] std::string_view name() const override { return "Showcase"; }
    Status init(Engine& engine, Services& services) override;
    void preUpdate(Engine& engine, const FrameTime& time) override;
    void onWorldUnloading(Engine& engine, World& world) override;
    void onWorldChanged(Engine& engine, World& world) override;
    void shutdown(Engine& engine, Services& services) override;

    [[nodiscard]] Engine& engine() { return *m_engine; }
    [[nodiscard]] const LaunchArgs& args() const { return m_args; }
    [[nodiscard]] const std::vector<StationInfo>& stations() const { return m_stations; }
    [[nodiscard]] NetDemo* netDemo() { return m_net.get(); }
    [[nodiscard]] Tour* tour() { return m_tour.get(); }

    // Switches a running game into the guided tour (main menu "Экскурсия с гидом").
    void startTour();
    // The tour reached its end without --quit: back to normal play in the hub.
    void finishTour();

    // Deferred engine actions requested by scripts (applied at the start of the next frame).
    void queueSave(std::string slot, std::string displayName);
    void queueLoad(std::string slot);
    void queueLevel(std::string uri);
    // Seconds since the last save written by the game (manual/quick/auto); large when none.
    [[nodiscard]] f64 secondsSinceSave() const;
    [[nodiscard]] const std::string& lastSaveKind() const { return m_lastSaveKind; }
    [[nodiscard]] const std::string& lastSaveError() const { return m_lastSaveError; }

    // Publishes a script event (Lua `events.subscribe(name, fn)`).
    void publish(const std::string& name, const std::vector<std::pair<std::string, std::string>>& fields);

private:
    void loadStations();
    void applyPending();

    Engine* m_engine = nullptr;
    Services* m_services = nullptr;
    script::ScriptVM* m_vm = nullptr;
    LaunchArgs m_args;
    std::vector<StationInfo> m_stations;
    std::unique_ptr<NetDemo> m_net;
    std::unique_ptr<Tour> m_tour;

    struct PendingSave {
        std::string slot, name;
    };
    std::vector<PendingSave> m_pendingSaves;
    std::optional<std::string> m_pendingLoad;
    std::optional<std::string> m_pendingLevel;
    f64 m_realTime = 0.0;
    f64 m_lastSaveAt = -1e9;
    f64 m_autosaveTimer = 0.0;
    std::string m_lastSaveKind;
    std::string m_lastSaveError;
};

// The module instance of the running engine (nullptr when the project does not enable it).
ShowcaseModule* activeModule();

void bindShowcaseLua(ShowcaseModule& module, script::ScriptVM& vm);

} // namespace ox::showcase
