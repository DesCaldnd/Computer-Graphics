#include "core/play_session.hpp"

#include <oxwald/core/log.hpp>

namespace ox::editor {

PlaySession::PlaySession(World& editWorld, EditorServices& services, QObject* parent)
    : QObject(parent), m_editWorld(editWorld), m_services(services) {
    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setInterval(16);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        const double dt = m_clock.isValid() ? std::min(0.1, double(m_clock.restart()) / 1000.0) : 1.0 / 60.0;
        if (!m_clock.isValid()) m_clock.start();
        if (m_state == State::Running) tick(dt);
    });
}

PlaySession::~PlaySession() { stop(); }

bool PlaySession::start(PlayMode mode) {
    if (m_state != State::Stopped) return false;
    m_mode = mode;
    m_world = m_editWorld.clone();
    m_world->updateTransforms();
    m_scheduler = std::make_unique<SystemScheduler>(1.0 / 60.0, 8);
    m_scheduler->emplace<TransformSystem>();
    for (const auto& rt : m_services.playRuntimes()) rt->begin(*m_world, *m_scheduler, m_services.engine(), mode);
    m_scheduler->setPlaying(mode == PlayMode::Play);
    m_scheduler->attach(*m_world, m_services.engine());
    m_elapsed = 0.0;
    m_frames = 0;
    m_state = State::Running;
    m_clock.start();
    if (m_autoTick) m_timer.start();
    OX_LOG_INFO("editor", "{} started ({} entities)", mode == PlayMode::Play ? "Play" : "Simulate", m_world->entityCount());
    Q_EMIT stateChanged();
    return true;
}

void PlaySession::stop() {
    if (m_state == State::Stopped) return;
    m_timer.stop();
    for (const auto& rt : m_services.playRuntimes()) rt->end(*m_world, m_services.engine());
    m_scheduler->detach();
    m_scheduler.reset();
    m_world.reset();
    m_state = State::Stopped;
    OX_LOG_INFO("editor", "Play stopped after {:.1f} s ({} frames)", m_elapsed, m_frames);
    Q_EMIT stateChanged();
}

void PlaySession::setPaused(bool paused) {
    if (m_state == State::Stopped) return;
    const State next = paused ? State::Paused : State::Running;
    if (next == m_state) return;
    m_state = next;
    if (!paused) m_clock.restart();
    Q_EMIT stateChanged();
}

void PlaySession::step() {
    if (m_state == State::Stopped) {
        if (!start(PlayMode::Play)) return;
        setPaused(true);
    }
    if (m_state != State::Paused) setPaused(true);
    const State keep = m_state;
    m_state = State::Running;
    tick(m_scheduler->fixedTimestep().fixedDt());
    m_state = keep;
}

void PlaySession::tick(double dt) {
    if (m_state != State::Running || !m_world) return;
    m_scheduler->tick(*m_world, m_services.engine(), dt);
    m_elapsed += dt;
    ++m_frames;
    Q_EMIT ticked();
}

} // namespace ox::editor
