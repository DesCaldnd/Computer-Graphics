#pragma once

#include "integration/editor_services.hpp"
#include "integration/runtime_host.hpp"

#include <oxwald/scene/system.hpp>
#include <oxwald/scene/world.hpp>

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include <memory>

namespace ox::editor {

// Play-in-editor. With the runtime module the session drives the editor's Engine: Play/Simulate =
// Engine::enterPlayMode() (the edit world is cloned with World::clone, UUIDs kept so the selection still
// resolves) + the IPlayRuntimes (gameplay: simulate-only configuration), Pause/Step = Engine::setPaused/stepFrames,
// Stop = exitPlayMode() — the edit world is never touched. Without the runtime it clones the world and ticks its
// own SystemScheduler (TransformSystem + IPlayRuntimes).
// The session's timer also ticks the engine in edit mode (setEditTicking) so edit-mode systems (debug draw,
// terrain, asset hot reload) run while the editor is open.
class PlaySession : public QObject {
    Q_OBJECT
public:
    enum class State { Stopped, Running, Paused };

    PlaySession(RuntimeHost& host, EditorServices& services, QObject* parent = nullptr);
    ~PlaySession() override;

    bool start(PlayMode mode);
    void stop();
    void setPaused(bool paused);
    // Advances exactly one fixed step while paused (or starts paused if stopped).
    void step();
    // Advances the simulation (the session's timer calls it; tests call it directly with autoTick off).
    void tick(double dt);
    void setAutoTick(bool enabled);
    // Ticks the engine in edit mode from the session timer (the main window enables it).
    void setEditTicking(bool enabled);
    [[nodiscard]] bool editTicking() const { return m_editTicking; }

    [[nodiscard]] State state() const { return m_state; }
    [[nodiscard]] bool active() const { return m_state != State::Stopped; }
    [[nodiscard]] PlayMode mode() const { return m_mode; }
    [[nodiscard]] World* world() const;
    [[nodiscard]] SystemScheduler* scheduler() const;
    [[nodiscard]] Services& services() const;
    [[nodiscard]] double elapsedSeconds() const { return m_elapsed; }
    [[nodiscard]] u64 frames() const { return m_frames; }
    [[nodiscard]] bool usesEngine() const { return m_host.engine() != nullptr; }

Q_SIGNALS:
    void stateChanged();
    void ticked();

private:
    void updateTimer();

    RuntimeHost& m_host;
    EditorServices& m_services;
    // fallback (no runtime module)
    std::unique_ptr<World> m_world;
    std::unique_ptr<SystemScheduler> m_scheduler;
    State m_state = State::Stopped;
    PlayMode m_mode = PlayMode::Play;
    QTimer m_timer;
    QElapsedTimer m_clock;
    bool m_autoTick = true;
    bool m_editTicking = false;
    double m_elapsed = 0.0;
    u64 m_frames = 0;
};

} // namespace ox::editor
