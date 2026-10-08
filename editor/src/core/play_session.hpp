#pragma once

#include "integration/editor_services.hpp"

#include <oxwald/scene/system.hpp>
#include <oxwald/scene/world.hpp>

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include <memory>

namespace ox::editor {

// Play-in-editor: Play clones the edit world (World::clone keeps UUIDs, so the selection still resolves) and
// ticks a SystemScheduler on the copy; Stop drops it — the edit world is never touched.
class PlaySession : public QObject {
    Q_OBJECT
public:
    enum class State { Stopped, Running, Paused };

    PlaySession(World& editWorld, EditorServices& services, QObject* parent = nullptr);
    ~PlaySession() override;

    bool start(PlayMode mode);
    void stop();
    void setPaused(bool paused);
    // Advances exactly one fixed step while paused (or starts paused if stopped).
    void step();
    // Advances the simulation (the session's timer calls it; tests call it directly with autoTick off).
    void tick(double dt);
    void setAutoTick(bool enabled) { m_autoTick = enabled; }

    [[nodiscard]] State state() const { return m_state; }
    [[nodiscard]] bool active() const { return m_state != State::Stopped; }
    [[nodiscard]] PlayMode mode() const { return m_mode; }
    [[nodiscard]] World* world() const { return m_world.get(); }
    [[nodiscard]] SystemScheduler* scheduler() const { return m_scheduler.get(); }
    [[nodiscard]] double elapsedSeconds() const { return m_elapsed; }
    [[nodiscard]] u64 frames() const { return m_frames; }

Q_SIGNALS:
    void stateChanged();
    void ticked();

private:
    World& m_editWorld;
    EditorServices& m_services;
    std::unique_ptr<World> m_world;
    std::unique_ptr<SystemScheduler> m_scheduler;
    State m_state = State::Stopped;
    PlayMode m_mode = PlayMode::Play;
    QTimer m_timer;
    QElapsedTimer m_clock;
    bool m_autoTick = true;
    double m_elapsed = 0.0;
    u64 m_frames = 0;
};

} // namespace ox::editor
