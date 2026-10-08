#include "integration/runtime_host.hpp"

#include "core/project.hpp"

#include <oxwald/core/debug_draw.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/scene/world.hpp>

#if OX_EDITOR_HAS_RUNTIME
#include <oxwald/core/events.hpp>
#include <oxwald/runtime/engine.hpp>
#endif

#include <QDir>
#include <QGuiApplication>

namespace ox::editor {

struct RuntimeHost::Impl {
#if OX_EDITOR_HAS_RUNTIME
    std::unique_ptr<Engine> engine;
    std::vector<ScopedConnection> connections;
#endif
};

RuntimeHost::RuntimeHost() : m_impl(std::make_unique<Impl>()), m_fallbackWorld(std::make_unique<World>()) {}

RuntimeHost::~RuntimeHost() { stop(); }

bool RuntimeHost::compiledIn() { return OX_EDITOR_HAS_RUNTIME != 0; }

bool RuntimeHost::running() const {
#if OX_EDITOR_HAS_RUNTIME
    return m_impl->engine && m_impl->engine->initialized();
#else
    return false;
#endif
}

Engine* RuntimeHost::engine() const {
#if OX_EDITOR_HAS_RUNTIME
    return running() ? m_impl->engine.get() : nullptr;
#else
    return nullptr;
#endif
}

Services* RuntimeHost::services() const {
#if OX_EDITOR_HAS_RUNTIME
    return running() ? &m_impl->engine->services() : nullptr;
#else
    return nullptr;
#endif
}

World& RuntimeHost::editWorld() {
#if OX_EDITOR_HAS_RUNTIME
    if (running()) return m_impl->engine->editWorld();
#endif
    return *m_fallbackWorld;
}

DebugDraw* RuntimeHost::gameDebugDraw() const {
#if OX_EDITOR_HAS_RUNTIME
    if (running()) return m_impl->engine->services().tryGet<DebugDraw>();
#endif
    return nullptr;
}

bool RuntimeHost::start(const Project* project, std::unique_ptr<World> world, QString* error) {
    if (!world) world = std::make_unique<World>();
#if OX_EDITOR_HAS_RUNTIME
    stop();
    EngineConfig cfg;
    cfg.appName = "OxwaldEditor";
    cfg.editor = true;
    const QString platform = QGuiApplication::platformName();
    cfg.headless = qEnvironmentVariableIsSet("OX_EDITOR_HEADLESS") || platform == QLatin1String("offscreen") ||
                   platform == QLatin1String("minimal");
    cfg.threadedRendering = false; // the viewport renders on the UI thread (IViewportRenderer)
    cfg.startupScene = "-";        // the editor opens its own startup scene
    cfg.loadUserSettings = false;  // project defaults drive the editor session; user settings are edited explicitly
    cfg.saveUserSettingsOnShutdown = false;
    cfg.fileWatching = true;
    if (project) cfg.projectPath = project->projectFile().toStdString();
    if (const QString userDir = qEnvironmentVariable("OX_EDITOR_USER_DIR"); !userDir.isEmpty()) {
        cfg.userDir = QDir(userDir).filePath(project ? project->name() : QStringLiteral("NoProject")).toStdString();
    }
    auto engine = std::make_unique<Engine>();
    if (auto st = engine->init(cfg); !st) {
        const QString msg = QString::fromStdString(st.error().message);
        OX_LOG_ERROR("editor", "engine init failed: {}", st.error().message);
        if (error) *error = msg;
        m_fallbackWorld = std::move(world);
        return false;
    }
    m_impl->engine = std::move(engine);
    Engine& e = *m_impl->engine;
    // "quit" from the console stops play-in-editor instead of the process.
    e.console().removeCommand("quit");
    e.console().addCommand("quit", "Stops play-in-editor", [this](auto) {
        if (m_onQuit) QMetaObject::invokeMethod(this, [this] { if (m_onQuit) m_onQuit(); }, Qt::QueuedConnection);
        return std::string("stopping play");
    });
    m_impl->connections.emplace_back(e.levelLoaded.connect([this](const std::string&) {
        if (!m_installing) QMetaObject::invokeMethod(this, &RuntimeHost::worldReplaced, Qt::QueuedConnection);
    }));
    m_installing = true;
    e.setWorld(std::move(world));
    m_installing = false;
    m_fallbackWorld = std::make_unique<World>();
    m_frames = 0;
    Q_EMIT started();
    return true;
#else
    (void)project;
    m_fallbackWorld = std::move(world);
    if (error) *error = QStringLiteral("runtime module not linked");
    return false;
#endif
}

void RuntimeHost::stop() {
#if OX_EDITOR_HAS_RUNTIME
    if (!m_impl->engine) return;
    Q_EMIT aboutToStop();
    m_impl->connections.clear();
    if (m_impl->engine->mode() == EngineMode::Play) m_impl->engine->exitPlayMode();
    m_impl->engine->shutdown();
    m_impl->engine.reset();
#endif
}

void RuntimeHost::setEditWorld(std::unique_ptr<World> world, const QString& level) {
    if (!world) world = std::make_unique<World>();
#if OX_EDITOR_HAS_RUNTIME
    if (running()) {
        m_installing = true;
        m_impl->engine->setWorld(std::move(world), level.toStdString());
        m_installing = false;
        return;
    }
#endif
    (void)level;
    m_fallbackWorld = std::move(world);
}

void RuntimeHost::tick(double dt) {
#if OX_EDITOR_HAS_RUNTIME
    if (!running()) return;
    Engine& e = *m_impl->engine;
    e.tick(dt);
    ++m_frames;
    // Gameplay debug draw is produced in Extract; keep it stable until the next engine frame.
    if (auto* dd = e.services().tryGet<DebugDraw>()) dd->flush(f32(dt));
    Q_EMIT ticked();
#else
    (void)dt;
#endif
}

} // namespace ox::editor
