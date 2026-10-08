#include "core/play_session.hpp"

#include <oxwald/core/log.hpp>

#if OX_EDITOR_HAS_RUNTIME
#include <oxwald/runtime/engine.hpp>
#endif

namespace ox::editor {

PlaySession::PlaySession(RuntimeHost& host, EditorServices& services, QObject* parent)
    : QObject(parent), m_host(host), m_services(services) {
    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setInterval(16);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        const double dt = m_clock.isValid() ? std::min(0.1, double(m_clock.restart()) / 1000.0) : 1.0 / 60.0;
        if (!m_clock.isValid()) m_clock.start();
        if (active()) {
            if (m_autoTick) tick(dt);
        } else if (m_editTicking) {
            m_host.tick(dt);
            Q_EMIT ticked();
        }
    });
}

PlaySession::~PlaySession() { stop(); }

World* PlaySession::world() const {
#if OX_EDITOR_HAS_RUNTIME
    if (Engine* e = m_host.engine()) return active() ? &e->world() : nullptr;
#endif
    return m_world.get();
}

SystemScheduler* PlaySession::scheduler() const {
#if OX_EDITOR_HAS_RUNTIME
    if (Engine* e = m_host.engine()) return &e->scheduler();
#endif
    return m_scheduler.get();
}

Services& PlaySession::services() const {
    if (Services* s = m_host.services()) return *s;
    return m_services.engine();
}

void PlaySession::updateTimer() {
    const bool run = (active() && m_autoTick) || (!active() && m_editTicking);
    if (run && !m_timer.isActive()) {
        m_clock.start();
        m_timer.start();
    } else if (!run && m_timer.isActive()) {
        m_timer.stop();
    }
}

void PlaySession::setAutoTick(bool enabled) {
    m_autoTick = enabled;
    updateTimer();
}

void PlaySession::setEditTicking(bool enabled) {
    m_editTicking = enabled;
    updateTimer();
}

bool PlaySession::start(PlayMode mode) {
    if (m_state != State::Stopped) return false;
    m_mode = mode;
    bool simulatePhysics = false;
    for (const auto& rt : m_services.playRuntimes()) simulatePhysics |= rt->simulatesPhysics();
#if OX_EDITOR_HAS_RUNTIME
    if (Engine* e = m_host.engine()) {
        e->setPaused(false);
        e->setTimeScale(1.0);
        e->enterPlayMode();
        if (e->mode() != EngineMode::Play) return false;
        for (const auto& rt : m_services.playRuntimes()) rt->begin(e->world(), e->scheduler(), e->services(), mode);
        e->scheduler().setPlaying(mode == PlayMode::Play || simulatePhysics);
    } else
#endif
    {
        World& edit = m_host.editWorld();
        m_world = edit.clone();
        m_world->updateTransforms();
        m_scheduler = std::make_unique<SystemScheduler>(1.0 / 60.0, 8);
        m_scheduler->emplace<TransformSystem>();
        for (const auto& rt : m_services.playRuntimes()) rt->begin(*m_world, *m_scheduler, m_services.engine(), mode);
        m_scheduler->setPlaying(mode == PlayMode::Play || simulatePhysics);
        m_scheduler->attach(*m_world, m_services.engine());
    }
    m_elapsed = 0.0;
    m_frames = 0;
    m_state = State::Running;
    m_clock.start();
    updateTimer();
    OX_LOG_INFO("editor", "{} started ({} entities)", mode == PlayMode::Play ? "Play" : "Simulate", world() ? world()->entityCount() : 0);
    Q_EMIT stateChanged();
    return true;
}

void PlaySession::stop() {
    if (m_state == State::Stopped) return;
#if OX_EDITOR_HAS_RUNTIME
    if (Engine* e = m_host.engine()) {
        for (const auto& rt : m_services.playRuntimes()) rt->end(e->world(), e->services());
        e->setPaused(false);
        e->exitPlayMode();
    } else
#endif
    {
        for (const auto& rt : m_services.playRuntimes()) rt->end(*m_world, m_services.engine());
        m_scheduler->detach();
        m_scheduler.reset();
        m_world.reset();
    }
    m_state = State::Stopped;
    updateTimer();
    OX_LOG_INFO("editor", "Play stopped after {:.1f} s ({} frames)", m_elapsed, m_frames);
    Q_EMIT stateChanged();
}

void PlaySession::setPaused(bool paused) {
    if (m_state == State::Stopped) return;
    const State next = paused ? State::Paused : State::Running;
    if (next == m_state) return;
    m_state = next;
#if OX_EDITOR_HAS_RUNTIME
    if (Engine* e = m_host.engine()) e->setPaused(paused);
#endif
    if (!paused) m_clock.restart();
    Q_EMIT stateChanged();
}

void PlaySession::step() {
    if (m_state == State::Stopped) {
        if (!start(PlayMode::Play)) return;
        setPaused(true);
    }
    if (m_state != State::Paused) setPaused(true);
#if OX_EDITOR_HAS_RUNTIME
    if (Engine* e = m_host.engine()) {
        e->stepFrames(1);
        const double fixedDt = e->scheduler().fixedTimestep().fixedDt();
        m_host.tick(fixedDt);
        m_elapsed += fixedDt;
        ++m_frames;
        Q_EMIT ticked();
        return;
    }
#endif
    const State keep = m_state;
    m_state = State::Running;
    tick(m_scheduler->fixedTimestep().fixedDt());
    m_state = keep;
}

void PlaySession::tick(double dt) {
#if OX_EDITOR_HAS_RUNTIME
    if (m_host.engine()) {
        if (m_state == State::Stopped) return;
        m_host.tick(dt); // paused: the engine runs the frame with dt 0 (transforms, debug draw)
        if (m_state == State::Running) {
            m_elapsed += dt;
            ++m_frames;
        }
        Q_EMIT ticked();
        return;
    }
#endif
    if (m_state != State::Running || !m_world) return;
    m_scheduler->tick(*m_world, m_services.engine(), dt);
    m_elapsed += dt;
    ++m_frames;
    Q_EMIT ticked();
}

} // namespace ox::editor
