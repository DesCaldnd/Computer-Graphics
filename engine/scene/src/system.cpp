#include <oxwald/core/profile.hpp>
#include <oxwald/scene/system.hpp>
#include <oxwald/scene/world.hpp>

#include <algorithm>

namespace ox {

std::string_view phaseName(SystemPhase phase) {
    switch (phase) {
    case SystemPhase::PreUpdate: return "PreUpdate";
    case SystemPhase::FixedUpdate: return "FixedUpdate";
    case SystemPhase::Update: return "Update";
    case SystemPhase::PostUpdate: return "PostUpdate";
    case SystemPhase::Extract: return "Extract";
    default: return "?";
    }
}

void TransformSystem::update(SystemContext& ctx) { ctx.world.updateTransforms(); }

SystemScheduler::SystemScheduler(f64 fixedDt, u32 maxFixedSteps) : m_fixed(fixedDt, maxFixedSteps) {}

SystemScheduler::~SystemScheduler() { detach(); }

ISystem& SystemScheduler::add(std::unique_ptr<ISystem> system) {
    OX_ASSERT(system != nullptr, "null system");
    OX_ASSERT(entry(system->name()) == nullptr, "system '{}' added twice", system->name());
    OX_ASSERT(system->phase() < SystemPhase::Count, "bad system phase");
    ISystem& ref = *system;
    m_entries.push_back(Entry{std::move(system), m_sequence++, true, false, 0.0});
    if (m_world) {
        ref.onAttach(*m_world, *m_services);
        m_entries.back().attached = true;
    }
    sort();
    return ref;
}

bool SystemScheduler::remove(std::string_view name) {
    auto it = std::find_if(m_entries.begin(), m_entries.end(), [&](const Entry& e) { return e.system->name() == name; });
    if (it == m_entries.end()) return false;
    if (it->attached && m_world) it->system->onDetach(*m_world, *m_services);
    m_entries.erase(it);
    return true;
}

SystemScheduler::Entry* SystemScheduler::entry(std::string_view name) {
    for (auto& e : m_entries) {
        if (e.system->name() == name) return &e;
    }
    return nullptr;
}

const SystemScheduler::Entry* SystemScheduler::entry(std::string_view name) const {
    return const_cast<SystemScheduler*>(this)->entry(name);
}

ISystem* SystemScheduler::find(std::string_view name) const {
    const Entry* e = entry(name);
    return e ? e->system.get() : nullptr;
}

void SystemScheduler::setEnabled(std::string_view name, bool enabled) {
    if (Entry* e = entry(name)) e->enabled = enabled;
}

bool SystemScheduler::isEnabled(std::string_view name) const {
    const Entry* e = entry(name);
    return e && e->enabled;
}

f64 SystemScheduler::lastTimeMs(std::string_view name) const {
    const Entry* e = entry(name);
    return e ? e->lastMs : 0.0;
}

std::vector<ISystem*> SystemScheduler::systems(SystemPhase phase) const {
    std::vector<ISystem*> out;
    for (const auto& e : m_entries) {
        if (e.system->phase() == phase) out.push_back(e.system.get());
    }
    return out;
}

void SystemScheduler::sort() {
    std::stable_sort(m_entries.begin(), m_entries.end(), [](const Entry& a, const Entry& b) {
        if (a.system->phase() != b.system->phase()) return a.system->phase() < b.system->phase();
        if (a.system->order() != b.system->order()) return a.system->order() < b.system->order();
        return a.sequence < b.sequence;
    });
}

void SystemScheduler::attach(World& world, Services& services) {
    detach();
    m_world = &world;
    m_services = &services;
    for (auto& e : m_entries) {
        e.system->onAttach(world, services);
        e.attached = true;
    }
}

void SystemScheduler::detach() {
    if (!m_world) return;
    for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it) {
        if (it->attached) it->system->onDetach(*m_world, *m_services);
        it->attached = false;
    }
    m_world = nullptr;
    m_services = nullptr;
}

void SystemScheduler::runPhase(SystemPhase phase, SystemContext& ctx) {
    ctx.phase = phase;
    for (auto& e : m_entries) {
        if (e.system->phase() != phase || !e.enabled) continue;
        if (e.system->playModeOnly() && !ctx.playing) continue;
        OX_PROFILE_ZONE_N("System");
        Stopwatch sw;
        sw.start();
        e.system->update(ctx);
        // Accumulate across fixed steps within a frame.
        e.lastMs += sw.elapsedMs();
    }
}

void SystemScheduler::tick(World& world, Services& services, f64 frameDt) {
    OX_PROFILE_ZONE();
    for (auto& e : m_entries) e.lastMs = 0.0;
    world.snapshotPreviousTransforms();
    SystemContext ctx{world, services};
    ctx.dt = static_cast<f32>(frameDt);
    ctx.fixedDt = static_cast<f32>(m_fixed.fixedDt());
    ctx.frame = m_frame;
    ctx.playing = m_playing;

    runPhase(SystemPhase::PreUpdate, ctx);

    const u32 steps = m_fixed.advance(frameDt);
    for (u32 i = 0; i < steps; ++i) {
        SystemContext fixedCtx = ctx;
        fixedCtx.dt = ctx.fixedDt;
        fixedCtx.fixedStep = i;
        runPhase(SystemPhase::FixedUpdate, fixedCtx);
    }
    ctx.alpha = static_cast<f32>(m_fixed.alpha());

    runPhase(SystemPhase::Update, ctx);
    runPhase(SystemPhase::PostUpdate, ctx);
    runPhase(SystemPhase::Extract, ctx);
    world.flushDestroyed();
    ++m_frame;
}

} // namespace ox
