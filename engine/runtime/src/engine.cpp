#include <oxwald/core/debug_draw.hpp>
#include <oxwald/core/events.hpp>
#include <oxwald/core/file_watcher.hpp>
#include <oxwald/core/jobs.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/core/paths.hpp>
#include <oxwald/core/profile.hpp>
#include <oxwald/core/scalability.hpp>
#include <oxwald/core/serial/format.hpp>
#include <oxwald/core/vfs.hpp>
#include <oxwald/runtime/engine.hpp>
#include <oxwald/runtime/json_io.hpp>
#include <oxwald/runtime/platform.hpp>
#include <oxwald/scene/scene.hpp>
#include <oxwald/scene/scene_serializer.hpp>

#if OX_HAS_ASSETS
#include <oxwald/assets/pak.hpp>
#endif

#include <algorithm>
#include <format>
#include <thread>

namespace ox {

struct Engine::LoadState {
    std::string uri;
    std::atomic<bool> ready{false};
    std::optional<Result<serial::Document>> doc;
    JobHandle job;
};

namespace {

bool isUri(std::string_view s) { return s.find("://") != std::string_view::npos; }

std::pair<std::string, std::string> splitCVarAssignment(std::string_view text) {
    const auto pos = text.find_first_of("= ");
    if (pos == std::string_view::npos) return {std::string(text), {}};
    return {std::string(text.substr(0, pos)), std::string(text.substr(pos + 1))};
}

} // namespace

Engine::Engine() = default;
Engine::~Engine() { shutdown(); }

void Engine::addModule(std::unique_ptr<IEngineModule> module) {
    OX_ASSERT(!m_initialized, "addModule after Engine::init");
    m_modules.push_back(std::move(module));
}

void Engine::setRenderer(std::unique_ptr<IRenderer> renderer) {
    OX_ASSERT(!m_initialized, "setRenderer after Engine::init");
    m_pendingRenderer = std::move(renderer);
}

void Engine::setPlatform(IPlatform* platform) {
    OX_ASSERT(!m_initialized, "setPlatform after Engine::init");
    m_platform = platform;
}

IEngineModule* Engine::findModule(std::string_view name) const {
    for (const auto& m : m_modules) {
        if (m->name() == name) return m.get();
    }
    return nullptr;
}

World& Engine::world() { return m_playWorld ? *m_playWorld : *m_world; }

// ---- init / shutdown --------------------------------------------------------------------------------------------

Status Engine::init(const EngineConfig& config) {
    OX_PROFILE_ZONE();
    if (m_initialized) return makeError("engine already initialised");
    m_config = config;
    if (m_config.dedicatedServer) m_config.headless = true;
    m_quit.store(false);
    m_startTime = Clock::timePoint();
    m_hasLastTick = false;

    registerSceneTypes();
    registerSettingsTypes();
    registerSaveGameTypes();
    if (auto st = loadProject(); !st) return st;

    // Built-in modules first so user/gameplay modules can rely on their services.
    auto builtins = makeBuiltinModules(m_config, m_projectSettings);
    m_modules.insert(m_modules.begin(), std::make_move_iterator(builtins.begin()),
                     std::make_move_iterator(builtins.end()));
    for (auto& m : m_modules) m->registerTypes();

    if (auto st = createServices(); !st) {
        OX_LOG_ERROR("engine", "init failed: {}", st.error().message);
        shutdown();
        return st;
    }

    const f64 rate = m_config.fixedRate.value_or(m_projectSettings.physics.fixedRate);
    const u32 maxSteps = m_config.maxFixedSteps.value_or(m_projectSettings.physics.maxSubsteps);
    m_scheduler = std::make_unique<SystemScheduler>(1.0 / std::max(rate, 1.0), std::max(maxSteps, 1u));
    m_scheduler->emplace<TransformSystem>();
    for (auto& m : m_modules) m->registerSystems(*this, *m_scheduler);
    m_mode = m_config.editor ? EngineMode::Edit : EngineMode::Play;
    m_scheduler->setPlaying(m_mode == EngineMode::Play);

    installWorld(std::make_unique<World>(), {});
    std::string scene = m_config.startupScene.empty() ? m_projectSettings.startupScene : m_config.startupScene;
    if (scene == "-") scene.clear();
    if (!scene.empty()) {
        if (auto st = loadScene(scene); !st) OX_LOG_ERROR("engine", "startup scene: {}", st.error().message);
    }

    if (!m_pendingRenderer) m_pendingRenderer = std::make_unique<NullRenderer>();
    m_renderer = &m_services.add<IRenderer>(std::move(m_pendingRenderer));
    const RenderSurface surface = m_platform ? m_platform->surface() : m_config.surface;
    m_lastFramebuffer = surface.framebufferSize;
    if (auto st = m_renderer->init(m_services, surface); !st) {
        OX_LOG_ERROR("engine", "renderer '{}' failed: {}", m_renderer->name(), st.error().message);
        shutdown();
        return st;
    }
    m_rendererInitialized = true;
    if (!m_config.dedicatedServer) {
        m_pipeline.start(*m_renderer, m_config.threadedRendering ? RenderPipeline::Mode::Threaded
                                                                 : RenderPipeline::Mode::SingleThreaded);
    }

    m_connections.emplace_back(m_settings->changed.connect([this](SettingsCategory c) {
        if (hasCategory(c, SettingsCategory::Graphics)) applyGraphicsSettings();
    }));
    m_connections.emplace_back(m_console->cvarChanged.connect([this](const std::string& name) {
        if (name.starts_with("r.") || name.starts_with("sg.")) {
            m_settings->captureFromCVars();
            applyGraphicsSettings();
        }
    }));
    m_connections.emplace_back(
        m_input->rebindsChanged.connect([this] { m_settings->setInputRebinds(m_input->rebinds()); }));

    m_initialized = true;
    OX_LOG_INFO("engine", "initialised '{}' ({} modules, renderer {}, {} rendering, {} job threads)",
                m_projectSettings.name, m_initializedModules.size(), m_renderer->name(),
                m_config.threadedRendering ? "threaded" : "single-threaded", m_jobs->threadCount());
    return {};
}

Status Engine::loadProject() {
#if OX_HAS_ASSETS
    if (m_config.projectPath.empty() && !m_config.pakPath.empty() && !m_config.projectSettings) {
        // Cooked game without a project directory: the project file travels inside the pak (cookProject).
        auto pak = assets::PakReader::open(m_config.pakPath);
        if (!pak) return makeError("pak '{}': {}", m_config.pakPath.string(), pak.error().message);
        for (const auto& entry : (*pak)->entries()) {
            if (entry.path.find('/') != std::string::npos || !entry.path.ends_with(Project::kExtension)) continue;
            auto bytes = (*pak)->read(entry);
            if (!bytes) return makeError("pak project file: {}", bytes.error().message);
            auto j = json::parse(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
            registerProjectTypes();
            ProjectSettings settings;
            if (!j || !json::fromPlain(*j, settings)) return makeError("pak project file '{}' is invalid", entry.path);
            m_projectSettings = std::move(settings);
            return {};
        }
        m_projectSettings = ProjectSettings{};
        m_projectSettings.name = m_config.appName;
        return {};
    }
#endif
    if (!m_config.projectPath.empty()) {
        auto p = Project::load(m_config.projectPath);
        if (!p) return makeError("project: {}", p.error().message);
        m_project = std::move(*p);
        m_projectSettings = m_project->settings;
    } else if (m_config.projectSettings) {
        m_projectSettings = *m_config.projectSettings;
    } else {
        m_projectSettings = ProjectSettings{};
        m_projectSettings.name = m_config.appName;
    }
    return {};
}

void Engine::mountFileSystems() {
    std::error_code ec;
    const auto engineDir = m_config.engineDir.empty() ? paths::engineSourceDir() : m_config.engineDir;
    m_vfs->mount("engine", std::make_unique<DirectoryMount>(engineDir, false));
    if (m_project) {
        // Loose project files; cooked builds mount a pak over it (assets module) at a higher priority.
        m_vfs->mount("project", std::make_unique<DirectoryMount>(m_project->root(), m_config.editor));
    }
    const auto userDir =
        m_config.userDir.empty() ? paths::userDataDir(m_project ? m_projectSettings.name : m_config.appName) : m_config.userDir;
    std::filesystem::create_directories(userDir, ec);
    m_vfs->mount("user", std::make_unique<DirectoryMount>(userDir, true));
}

Status Engine::createServices() {
    OX_PROFILE_ZONE();
    // Registration order = dependency order; Services destroys in reverse.
    m_jobs = &m_services.emplace<JobSystem>(m_config.workerThreads);
    m_services.emplace<EventBus>();
    m_services.emplace<FileWatcher>();
    m_vfs = &m_services.emplace<Vfs>();
    mountFileSystems();
    m_services.emplace<DebugDraw>();

    m_settings = &m_services.emplace<Settings>(m_vfs);
    m_settings->setProject(m_projectSettings);
    m_settings->applyProjectDefaults();
    if (m_config.loadUserSettings) {
        if (auto st = m_settings->load(); !st) OX_LOG_WARN("engine", "user settings: {}", st.error().message);
    }
    m_settings->apply();
    applyCommandLineOverrides();

    m_input = &m_services.emplace<InputSystem>();
    m_input->setMappings(m_projectSettings.input);
    m_input->applyRebinds(m_settings->user().inputRebinds);

    SaveGameConfig saveConfig;
    saveConfig.version = m_projectSettings.saveVersion;
    saveConfig.gameVersion = m_projectSettings.version;
    m_saves = &m_services.emplace<SaveGameSystem>(saveConfig, m_jobs, m_vfs);
    m_saves->setWorldProvider([this]() -> World* { return m_world ? &world() : nullptr; });
    m_saves->setLevelLoader([this](const std::string& level) -> World* {
        if (auto st = loadScene(level); !st) {
            OX_LOG_ERROR("engine", "save game level: {}", st.error().message);
            return nullptr;
        }
        return &world();
    });

    m_console = &m_services.emplace<Console>();
    m_console->addCommand("quit", "Quits the game", [this](auto) {
        requestQuit();
        return std::string("quitting");
    });
    m_console->addCommand("pause", "Toggles pause", [this](auto) {
        setPaused(!paused());
        return std::string(paused() ? "paused" : "running");
    });
    m_console->addCommand("step", "Advances N paused frames (default 1)", [this](std::span<const std::string> a) {
        stepFrames(a.empty() ? 1u : u32(std::max(1, std::atoi(a[0].c_str()))));
        return std::string();
    });
    m_console->addCommand("timescale", "Sets the game time scale", [this](std::span<const std::string> a) {
        if (!a.empty()) setTimeScale(std::atof(a[0].c_str()));
        return std::format("timescale {}", timeScale());
    });
    m_console->addCommand("level", "Loads a level (async)", [this](std::span<const std::string> a) {
        if (a.empty()) return std::format("current level: {}", m_level);
        requestLevelChange(a[0]);
        return std::format("loading {}", a[0]);
    });
    m_console->addCommand("save", "Saves the game to a slot", [this](std::span<const std::string> a) {
        auto r = saveGame(a.empty() ? "console" : a[0]);
        return r ? std::format("saved {} ({} bytes)", r->slot, r->bytes) : r.error().message;
    });
    m_console->addCommand("load", "Loads a save slot", [this](std::span<const std::string> a) {
        auto r = loadGame(a.empty() ? "console" : a[0]);
        return r ? std::format("loaded {}", r->header.slot) : r.error().message;
    });
    m_console->addCommand("stats", "Frame statistics", [this](auto) {
        return std::format("frame {} | {:.2f} ms ({:.0f} fps) | game {:.2f} ms | render {:.2f} ms | fixed {} | "
                           "alpha {:.2f}",
                           m_stats.frame, m_stats.frameMs, m_stats.fps, m_stats.gameMs, m_stats.renderMs,
                           m_stats.fixedSteps, m_stats.alpha);
    });

    for (auto& m : m_modules) {
        OX_PROFILE_ZONE_N("ModuleInit");
        if (auto st = m->init(*this, m_services); !st) {
            return makeError("module '{}': {}", m->name(), st.error().message);
        }
        m_initializedModules.push_back(m.get());
    }
    return {};
}

void Engine::applyCommandLineOverrides() {
    if (m_config.quality && *m_config.quality != QualityLevel::Custom) scalability::setOverall(*m_config.quality);
    for (const auto& assignment : m_config.cvars) {
        auto [name, value] = splitCVarAssignment(assignment);
        if (name.empty()) continue;
        if (ICVar* c = CVarRegistry::instance().find(name)) {
            if (!c->setFromString(value, CVarSource::Config)) OX_LOG_WARN("engine", "cannot set cvar {}={}", name, value);
        } else {
            nlohmann::json j = nlohmann::json::object();
            j[name] = value;
            CVarRegistry::instance().loadOverrides(j); // pending until registered
        }
    }
}

void Engine::shutdown() {
    const bool anything = m_initialized || !m_initializedModules.empty() || m_services.size() > 0;
    if (!anything) return;
    OX_PROFILE_ZONE();
    m_pipeline.stop();
    if (m_rendererInitialized) {
        m_renderer->shutdown();
        m_rendererInitialized = false;
    }
    if (m_loadState && m_jobs) m_jobs->wait(m_loadState->job);
    m_loadState.reset();
    if (m_saves) m_saves->waitIdle();
    if (m_initialized && m_settings && m_config.saveUserSettingsOnShutdown) {
        m_settings->captureFromCVars();
        if (m_input) m_settings->user().inputRebinds = m_input->rebinds();
        if (auto st = m_settings->save(); !st) OX_LOG_WARN("engine", "saving user settings: {}", st.error().message);
    }
    m_connections.clear();
    if (m_world) {
        for (IEngineModule* m : m_initializedModules) m->onWorldUnloading(*this, world());
    }
    detachWorld();
    // Worlds go before module services: component destructors/hooks may still talk to them (physics bodies).
    m_playWorld.reset();
    m_world.reset();
    m_additive.clear();
    m_scheduler.reset();
    for (auto it = m_initializedModules.rbegin(); it != m_initializedModules.rend(); ++it) {
        (*it)->shutdown(*this, m_services);
    }
    m_initializedModules.clear();
    m_services.clear(); // reverse registration order: renderer ... job system
    m_modules.clear();
    m_renderer = nullptr;
    m_jobs = nullptr;
    m_vfs = nullptr;
    m_input = nullptr;
    m_settings = nullptr;
    m_saves = nullptr;
    m_console = nullptr;
    m_initialized = false;
    OX_LOG_INFO("engine", "shut down");
}

// ---- worlds -----------------------------------------------------------------------------------------------------

void Engine::attachWorld() {
    if (!m_scheduler || !m_world) return;
    World& w = world();
    m_scheduler->attach(w, m_services);
    m_attachedWorld = &w;
    for (IEngineModule* m : m_initializedModules) m->onWorldChanged(*this, w);
}

void Engine::detachWorld() {
    if (m_scheduler && m_attachedWorld) m_scheduler->detach();
    m_attachedWorld = nullptr;
}

void Engine::installWorld(std::unique_ptr<World> newWorld, const std::string& level) {
    OX_PROFILE_ZONE();
    if (m_playWorld) {
        for (IEngineModule* m : m_initializedModules) m->onWorldUnloading(*this, *m_playWorld);
        detachWorld();
        m_playWorld.reset();
        m_mode = EngineMode::Edit;
        if (m_scheduler) m_scheduler->setPlaying(false);
        modeChanged.emit(m_mode);
    }
    detachWorld();
    if (m_world) {
        for (IEngineModule* m : m_initializedModules) m->onWorldUnloading(*this, *m_world);
        if (!m_level.empty()) levelUnloading.emit(m_level);
    }
    m_world = std::move(newWorld);
    m_level = level;
    m_additive.clear();
    if (m_scheduler) m_scheduler->fixedTimestep().reset();
    if (m_saves) {
        m_saves->setCurrentLevel(level);
        m_saves->trackLevelEntities(*m_world);
    }
    attachWorld();
    levelLoaded.emit(m_level);
}

void Engine::setWorld(std::unique_ptr<World> world, std::string levelUri) {
    OX_ASSERT(world != nullptr, "setWorld(null)");
    installWorld(std::move(world), levelUri);
}

Result<serial::Document> Engine::readSceneDocument(std::string_view uriOrPath) const {
    OX_PROFILE_ZONE();
    Result<std::vector<std::byte>> bytes =
        isUri(uriOrPath) ? m_vfs->readBytes(uriOrPath) : serial::readFileBytes(std::filesystem::path(uriOrPath));
    if (!bytes) return makeError("cannot read scene '{}': {}", uriOrPath, bytes.error().message);
    auto doc = serial::decodeAny(*bytes);
    if (!doc) return makeError("scene '{}': {}", uriOrPath, doc.error().message);
    return doc;
}

Result<std::unique_ptr<World>> Engine::buildWorld(const serial::Document& doc) const {
    OX_PROFILE_ZONE();
    auto w = std::make_unique<World>();
    if (auto st = deserializeWorld(*w, doc); !st) return st.error();
    return w;
}

Status Engine::loadScene(std::string_view uriOrPath) {
    OX_PROFILE_ZONE();
    auto doc = readSceneDocument(uriOrPath);
    if (!doc) return doc.error();
    auto w = buildWorld(*doc);
    if (!w) return makeError("scene '{}': {}", uriOrPath, w.error().message);
    installWorld(std::move(*w), std::string(uriOrPath));
    return {};
}

Result<std::vector<Entity>> Engine::loadSceneAdditive(std::string_view uriOrPath) {
    OX_PROFILE_ZONE();
    auto doc = readSceneDocument(uriOrPath);
    if (!doc) return doc.error();
    const serial::Value* entities = doc->root.find("entities");
    if (!entities) return makeError("scene '{}' has no entities", uriOrPath);
    // UUIDs are kept so save games and references can address streamed entities.
    auto r = instantiateEntities(world(), *entities, false);
    if (!r) return makeError("scene '{}': {}", uriOrPath, r.error().message);
    AdditiveScene a{std::string(uriOrPath), {}};
    for (const Entity& e : r->roots) a.roots.push_back(e.uuid());
    m_additive.push_back(std::move(a));
    return r->roots;
}

Status Engine::unloadAdditive(std::string_view uriOrPath) {
    auto it = std::find_if(m_additive.begin(), m_additive.end(), [&](const AdditiveScene& a) { return a.uri == uriOrPath; });
    if (it == m_additive.end()) return makeError("scene '{}' is not loaded additively", uriOrPath);
    for (const Uuid& id : it->roots) {
        if (Entity e = world().find(id); e.valid()) world().destroyImmediate(e);
    }
    m_additive.erase(it);
    return {};
}

std::vector<std::string> Engine::additiveScenes() const {
    std::vector<std::string> out;
    for (const auto& a : m_additive) out.push_back(a.uri);
    return out;
}

void Engine::requestLevelChange(std::string uriOrPath) { m_pendingLevel = std::move(uriOrPath); }

void Engine::updateLevelLoading() {
    if (!m_pendingLevel.empty() && !m_loadState) {
        auto state = std::make_shared<LoadState>();
        state->uri = std::exchange(m_pendingLevel, {});
        if (m_loadingHooks.begin) m_loadingHooks.begin(state->uri);
        m_saves->onLevelChanged(state->uri); // autosave of the level being left
        state->job = m_jobs->submit([this, state] {
            OX_PROFILE_ZONE_N("LoadLevel");
            state->doc.emplace(readSceneDocument(state->uri));
            state->ready.store(true, std::memory_order_release);
        });
        m_loadState = std::move(state);
    }
    if (m_loadState && m_loadState->ready.load(std::memory_order_acquire)) {
        auto state = std::move(m_loadState);
        bool ok = false;
        if (*state->doc) {
            if (auto w = buildWorld(**state->doc)) {
                installWorld(std::move(*w), state->uri);
                ok = true;
            } else {
                OX_LOG_ERROR("engine", "level '{}': {}", state->uri, w.error().message);
            }
        } else {
            OX_LOG_ERROR("engine", "{}", state->doc->error().message);
        }
        if (m_loadingHooks.end) m_loadingHooks.end(state->uri, ok);
    }
}

// ---- modes ------------------------------------------------------------------------------------------------------

void Engine::enterPlayMode() {
    if (!m_config.editor || m_mode == EngineMode::Play || !m_world) return;
    OX_PROFILE_ZONE();
    detachWorld();
    m_playWorld = m_world->clone();
    m_mode = EngineMode::Play;
    m_scheduler->setPlaying(true);
    m_scheduler->fixedTimestep().reset();
    m_saves->trackLevelEntities(*m_playWorld);
    attachWorld();
    modeChanged.emit(m_mode);
}

void Engine::exitPlayMode() {
    if (!m_config.editor || m_mode != EngineMode::Play) return;
    OX_PROFILE_ZONE();
    if (m_playWorld) {
        for (IEngineModule* m : m_initializedModules) m->onWorldUnloading(*this, *m_playWorld);
    }
    detachWorld();
    m_playWorld.reset();
    m_mode = EngineMode::Edit;
    m_scheduler->setPlaying(false);
    m_saves->trackLevelEntities(*m_world);
    attachWorld();
    modeChanged.emit(m_mode);
}

// ---- save games -------------------------------------------------------------------------------------------------

Result<SaveResult> Engine::saveGame(std::string_view slot, std::string displayName) {
    return m_saves->save(slot, world(), SaveKind::Manual, std::move(displayName));
}

Result<LoadResult> Engine::loadGame(std::string_view slot) { return m_saves->load(slot); }

// ---- settings ---------------------------------------------------------------------------------------------------

void Engine::applyGraphicsSettings() {
    if (m_platform) m_platform->applyWindowSettings(m_settings->user().graphics);
    m_pipeline.requestSettingsChanged();
}

// ---- loop -------------------------------------------------------------------------------------------------------

f64 Engine::targetFps() const {
    if (m_config.targetFps > 0.0) return m_config.targetFps;
    return f64(cvars::maxFps().get());
}

void Engine::pace(Clock::TimePoint frameStart) {
    const f64 fps = targetFps();
    if (fps <= 0.0) return;
    OX_PROFILE_ZONE_N("FramePacing");
    const auto deadline = frameStart + std::chrono::duration_cast<Clock::Duration>(std::chrono::duration<f64>(1.0 / fps));
    // Coarse sleep, then yield for the last millisecond (sleep granularity is ~1 ms on most OSes).
    const auto coarse = deadline - std::chrono::milliseconds(1);
    if (Clock::timePoint() < coarse) std::this_thread::sleep_until(coarse);
    while (Clock::timePoint() < deadline) std::this_thread::yield();
}

bool Engine::tick() {
    const auto now = Clock::timePoint();
    const f64 dt = m_hasLastTick ? Clock::secondsBetween(m_lastTick, now) : 0.0;
    m_lastTick = now;
    m_hasLastTick = true;
    const bool keepRunning = frame(dt, false);
    const auto beforePace = Clock::timePoint();
    pace(now);
    m_stats.sleepMs = Clock::secondsBetween(beforePace, Clock::timePoint()) * 1000.0;
    return keepRunning;
}

bool Engine::tick(f64 realDt) { return frame(realDt, false); }

void Engine::run(u64 maxFrames) {
    u64 frames = 0;
    while (!m_quit.load()) {
        if (!tick()) break;
        if (maxFrames > 0 && ++frames >= maxFrames) break;
    }
}

bool Engine::frame(f64 realDt, bool) {
    OX_ASSERT(m_initialized, "Engine::tick before init");
    OX_PROFILE_ZONE_N("GameFrame");
    Stopwatch gameSw(true);
    if (!(realDt >= 0.0)) realDt = 0.0;
    realDt = std::min(realDt, m_config.maxFrameDelta);

    if (m_platform) {
        m_platform->pollEvents();
        if (m_platform->shouldClose()) m_quit.store(true);
        const glm::uvec2 fb = m_platform->framebufferSize();
        if (fb != m_lastFramebuffer) {
            m_lastFramebuffer = fb;
            m_pipeline.requestResize(fb);
        }
    }
    m_jobs->runMainThreadQueue();
    if (m_config.fileWatching) m_services.get<FileWatcher>().poll();
    m_services.get<EventBus>().dispatch();
    updateLevelLoading();
    m_input->update(realDt);

    f64 dt = realDt * m_timeScale;
    if (m_paused) {
        dt = 0.0;
        if (m_stepRequests > 0) {
            --m_stepRequests;
            dt = m_scheduler->fixedTimestep().fixedDt();
        }
    }
    FrameTime ft;
    ft.realDt = realDt;
    ft.dt = dt;
    ft.frameIndex = m_frameIndex;
    ft.timeScale = m_timeScale;
    ft.paused = m_paused;
    ft.stepping = m_paused && dt > 0.0;
    ft.playing = m_mode == EngineMode::Play && !loading();
    m_frameTime = ft;
    for (IEngineModule* m : m_initializedModules) m->preUpdate(*this, ft);

    FixedTimestep& fixed = m_scheduler->fixedTimestep();
    const u64 stepsBefore = fixed.totalSteps();
    if (!loading()) {
        OX_PROFILE_ZONE_N("Simulation");
        Stopwatch sim(true);
        m_scheduler->tick(world(), m_services, dt);
        m_stats.simulationMs = sim.elapsedMs();
        m_gameTime += dt;
    }
    m_saves->update(realDt, m_mode == EngineMode::Play && !m_paused && !loading());

    FrameContext ctx;
    ctx.frameIndex = m_frameIndex;
    ctx.time = m_gameTime;
    ctx.realTime = Clock::secondsBetween(m_startTime, Clock::timePoint());
    ctx.dt = f32(dt);
    ctx.realDt = f32(realDt);
    ctx.alpha = f32(fixed.alpha());
    ctx.timeScale = f32(m_timeScale);
    ctx.paused = m_paused;
    ctx.editMode = m_mode == EngineMode::Edit;
    ctx.loading = loading();
    ctx.viewportSize = m_lastFramebuffer;
    if (m_pipeline.running()) {
        m_pipeline.submit(world(), ctx);
        const auto ps = m_pipeline.stats();
        m_stats.extractMs = ps.lastExtractMs;
        m_stats.renderMs = ps.lastRenderMs;
        m_stats.waitForRenderMs = ps.lastWaitMs;
    }

    m_frameTimer.update(realDt);
    m_stats.frame = m_frameIndex;
    m_stats.fixedSteps = u32(fixed.totalSteps() - stepsBefore);
    m_stats.totalFixedSteps = fixed.totalSteps();
    m_stats.alpha = fixed.alpha();
    m_stats.droppedTime = fixed.droppedTime();
    m_stats.gameTime = m_gameTime;
    m_stats.realTime = ctx.realTime;
    m_stats.frameMs = realDt * 1000.0;
    m_stats.fps = m_frameTimer.fps();
    m_stats.gameMs = gameSw.elapsedMs();
    OX_PROFILE_PLOT("Game thread ms", m_stats.gameMs);
    ++m_frameIndex;
    frameEnded.emit(m_stats);
    OX_PROFILE_FRAME();
    return !m_quit.load();
}

} // namespace ox
