#include "showcase.hpp"

#include "net_demo.hpp"
#include "tour.hpp"

#include <oxwald/core/log.hpp>
#include <oxwald/core/vfs.hpp>
#include <oxwald/runtime/game_module.hpp>
#include <oxwald/script/script_events.hpp>
#include <oxwald/script/script_vm.hpp>

#include <nlohmann/json.hpp>

namespace ox::showcase {

namespace {
ShowcaseModule* g_active = nullptr;
constexpr f64 kAutosaveInterval = 180.0; // seconds of play time between autosaves
} // namespace

ShowcaseModule* activeModule() { return g_active; }

LaunchArgs parseLaunchArgs(const std::vector<std::string>& args, bool headless) {
    LaunchArgs a;
    a.raw = args;
    a.quitAfterTour = headless;
    for (usize i = 0; i < args.size(); ++i) {
        const std::string& s = args[i];
        if (s == "--tour") a.mode = LaunchMode::Tour;
        else if (s == "--photo") a.mode = LaunchMode::Photo;
        else if (s == "--quit") a.quitAfterTour = true;
        else if (s == "--stay") a.quitAfterTour = false;
        else if (s == "--shots" && i + 1 < args.size()) a.shotsDir = args[++i];
        else if (s == "--station" && i + 1 < args.size()) a.onlyStation = args[++i];
        else OX_LOG_WARN("showcase", "unknown game argument '{}' (known: --tour --photo --shots DIR --station ID --quit)", s);
    }
    return a;
}

ShowcaseModule::ShowcaseModule() = default;
ShowcaseModule::~ShowcaseModule() = default;

Status ShowcaseModule::init(Engine& engine, Services& services) {
    m_engine = &engine;
    m_services = &services;
    g_active = this;
    m_args = parseLaunchArgs(engine.config().gameArgs, engine.config().headless);
    loadStations();

    m_vm = services.tryGet<script::ScriptVM>();
    if (m_vm) {
        if (engine.project()) m_vm->addSearchRoot(engine.project()->root() / "Assets" / "Scripts");
        bindShowcaseLua(*this, *m_vm);
    }
    m_net = std::make_unique<NetDemo>(*this);
    m_tour = std::make_unique<Tour>(*this);
    engine.saves().setAutosaveInterval(0.0); // the module autosaves itself so it can show the indicator
    OX_LOG_INFO("showcase", "Showcase module ready ({} stations, mode {})", m_stations.size(),
                m_args.mode == LaunchMode::Tour ? "tour" : m_args.mode == LaunchMode::Photo ? "photo" : "play");
    return {};
}

void ShowcaseModule::loadStations() {
    m_stations.clear();
    auto text = m_engine->vfs().readText("project://Assets/Data/stations.json");
    if (!text) {
        OX_LOG_WARN("showcase", "stations.json: {}", text.error().message);
        return;
    }
    const auto j = nlohmann::json::parse(*text, nullptr, false);
    if (!j.is_object() || !j.contains("stations")) return;
    for (const auto& s : j["stations"]) {
        m_stations.push_back({s.value("id", ""), s.value("title", ""), s.value("scene", ""), s.value("guide", "")});
    }
}

void ShowcaseModule::startTour() {
    if (m_args.mode == LaunchMode::Tour) return;
    m_args.mode = LaunchMode::Tour;
    m_args.quitAfterTour = false;
    m_tour = std::make_unique<Tour>(*this);
}

void ShowcaseModule::finishTour() {
    m_args.mode = LaunchMode::Play;
    queueLevel("project://Assets/Scenes/Hub.oxscene");
}

void ShowcaseModule::queueSave(std::string slot, std::string displayName) {
    m_pendingSaves.push_back({std::move(slot), std::move(displayName)});
}
void ShowcaseModule::queueLoad(std::string slot) { m_pendingLoad = std::move(slot); }
void ShowcaseModule::queueLevel(std::string uri) { m_pendingLevel = std::move(uri); }
f64 ShowcaseModule::secondsSinceSave() const { return m_realTime - m_lastSaveAt; }

void ShowcaseModule::publish(const std::string& name, const std::vector<std::pair<std::string, std::string>>& fields) {
    if (!m_vm) return;
    sol::table t = m_vm->lua().create_table();
    for (const auto& [k, v] : fields) t[k] = v;
    m_vm->events().publish(name, t);
}

void ShowcaseModule::applyPending() {
    Engine& e = *m_engine;
    for (const PendingSave& s : m_pendingSaves) {
        auto r = e.saveGame(s.slot, s.name);
        if (r) {
            m_lastSaveAt = m_realTime;
            m_lastSaveKind = s.slot == "quicksave" ? "quick" : s.slot.starts_with("autosave") ? "auto" : "manual";
            m_lastSaveError.clear();
            OX_LOG_INFO("showcase", "saved '{}' ({} bytes)", s.slot, r->bytes);
        } else {
            m_lastSaveError = r.error().message;
            OX_LOG_WARN("showcase", "save '{}' failed: {}", s.slot, r.error().message);
        }
    }
    m_pendingSaves.clear();
    if (m_pendingLoad) {
        const std::string slot = *m_pendingLoad;
        m_pendingLoad.reset();
        if (auto r = e.loadGame(slot); r) {
            OX_LOG_INFO("showcase", "loaded '{}' ({} created, {} updated)", slot, r->entitiesCreated, r->entitiesUpdated);
            publish("showcase.loaded", {{"slot", slot}});
        } else {
            OX_LOG_WARN("showcase", "load '{}' failed: {}", slot, r.error().message);
            publish("showcase.loadFailed", {{"slot", slot}, {"error", r.error().message}});
        }
    }
    if (m_pendingLevel) {
        e.requestLevelChange(*m_pendingLevel);
        m_pendingLevel.reset();
    }
}

void ShowcaseModule::preUpdate(Engine& engine, const FrameTime& time) {
    m_realTime += time.realDt;
    applyPending();
    if (time.playing && m_args.mode == LaunchMode::Play && !engine.loading()) {
        m_autosaveTimer += time.dt;
        if (m_autosaveTimer >= kAutosaveInterval && engine.currentLevel().find("/Stations/") != std::string::npos) {
            m_autosaveTimer = 0.0;
            engine.saves().autosave();
            m_lastSaveAt = m_realTime;
            m_lastSaveKind = "auto";
        }
    }
    m_tour->preUpdate(time);
    m_net->preUpdate(time);
}

void ShowcaseModule::onWorldUnloading(Engine&, World& world) { m_net->stop(); (void)world; }

void ShowcaseModule::onWorldChanged(Engine& engine, World& world) {
    // Leaving a level autosaves it (SaveGameConfig::autosaveOnLevelChange): show the indicator.
    if (m_args.mode == LaunchMode::Play && !engine.currentLevel().empty()) {
        m_lastSaveAt = m_realTime;
        m_lastSaveKind = "auto";
    }
    m_net->onWorldChanged(world);
    m_tour->onWorldChanged(world);
}

void ShowcaseModule::shutdown(Engine&, Services&) {
    if (m_net) m_net->stop();
    m_tour.reset();
    m_net.reset();
    m_vm = nullptr;
    if (g_active == this) g_active = nullptr;
}

OX_GAME_MODULE("Showcase", ShowcaseModule);

} // namespace ox::showcase
