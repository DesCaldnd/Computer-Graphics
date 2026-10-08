#pragma once

#include <QObject>
#include <QString>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ox {
class DebugDraw;
class Engine;
class Services;
class World;
} // namespace ox

namespace ox::editor {

class Project;

// The editor's game engine: one ox::Engine in editor mode (EngineConfig::editor, single-threaded rendering with the
// NullRenderer — the viewport draws through IViewportRenderer), recreated when a project is opened. It owns the
// edit world, the services (AssetRegistry/AssetManager, physics, audio, script VM + async bridge, coroutine
// scheduler, input, settings, save games, console) and the gameplay systems; play-in-editor uses
// Engine::enterPlayMode()/exitPlayMode(). Builds without the runtime module fall back to a plain World.
class RuntimeHost : public QObject {
    Q_OBJECT
public:
    RuntimeHost();
    ~RuntimeHost() override;

    [[nodiscard]] static bool compiledIn();

    // (Re)creates the engine for `project` (nullptr = no project: no asset database). `world` becomes the edit
    // world (nullptr = empty world). On failure the host keeps a plain world and returns false.
    bool start(const Project* project, std::unique_ptr<World> world, QString* error = nullptr);
    void stop();
    [[nodiscard]] bool running() const;

    // Null without the runtime module / before start().
    [[nodiscard]] Engine* engine() const;
    [[nodiscard]] Services* services() const;

    [[nodiscard]] World& editWorld();
    // Replaces the edit world (scene open/new). `level` is the path/URI recorded as the current level.
    void setEditWorld(std::unique_ptr<World> world, const QString& level = {});

    // One engine frame with an explicit delta (edit mode: transforms, gameplay edit-mode systems, asset hot
    // reload, debug draw; play mode: simulation). The game debug draw is flushed afterwards.
    void tick(double dt);
    // Lines the gameplay systems submitted during the last tick (physics colliders, splines, navmesh, ...).
    [[nodiscard]] DebugDraw* gameDebugDraw() const;
    [[nodiscard]] unsigned long long frames() const { return m_frames; }

    // Called by the console "quit" command (the editor maps it to "stop play").
    void setQuitHandler(std::function<void()> fn) { m_onQuit = std::move(fn); }

Q_SIGNALS:
    void started();
    void aboutToStop();
    // The engine installed a different world by itself (console "level", save game load).
    void worldReplaced();
    void ticked();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::unique_ptr<World> m_fallbackWorld;
    std::function<void()> m_onQuit;
    unsigned long long m_frames = 0;
    bool m_installing = false;
};

} // namespace ox::editor
