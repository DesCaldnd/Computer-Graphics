#include <oxwald/gameplay/coroutines.hpp>

#if defined(OX_GAMEPLAY_HAS_ASYNC)

#include <oxwald/scene/components.hpp>

namespace ox::gameplay {

CoroutineHandle startCoroutine(Services& services, Entity e, Task<> task, std::string name) {
    auto* scheduler = services.tryGet<CoroutineScheduler>();
    if (!scheduler || !e.valid()) return {};
    SpawnOptions o;
    o.name = std::move(name);
    o.owner = coroutineOwner(e);
    return scheduler->spawn(std::move(task), std::move(o));
}

void stopAllCoroutines(Services& services, Entity e) {
    if (auto* scheduler = services.tryGet<CoroutineScheduler>(); scheduler && e.valid()) {
        scheduler->cancelOwner(coroutineOwner(e));
    }
}

CoroutineRuntime::~CoroutineRuntime() { detach(); }

void CoroutineRuntime::attach(World& world, Services& services) {
    detach();
    m_world = &world;
    m_scheduler = services.tryGet<CoroutineScheduler>();
    entt::registry& r = world.registry();
    // Deferred destroy (Entity::destroy) marks the subtree first: cancel there, while components still exist.
    m_connections.emplace_back(r.on_construct<PendingDestroyTag>().connect<&CoroutineRuntime::onPendingDestroy>(*this));
    // World::destroyImmediate / clear(): the Id component is among the first to go; cancelling is idempotent.
    m_connections.emplace_back(r.on_destroy<IdComponent>().connect<&CoroutineRuntime::onPendingDestroy>(*this));
}

void CoroutineRuntime::detach() {
    if (!m_world) return;
    cancelWorldOwners();
    m_connections.clear();
    m_world = nullptr;
    m_playing = false;
}

void CoroutineRuntime::onPendingDestroy(entt::registry&, entt::entity e) {
    if (m_scheduler) m_scheduler->cancelOwner(coroutineOwner(e));
}

void CoroutineRuntime::cancelWorldOwners() {
    if (!m_scheduler || !m_world) return;
    for (auto e : m_world->registry().view<IdComponent>()) m_scheduler->cancelOwner(coroutineOwner(e));
}

void CoroutineRuntime::syncPlayState(bool playing) {
    if (playing == m_playing) return;
    m_playing = playing;
    if (!playing) cancelWorldOwners();
}

void CoroutineRuntime::tick(f32 dt, u64 frame) {
    if (m_scheduler) m_scheduler->tick(dt, frame);
}

void CoroutineRuntime::fixedTick(f32 dt) {
    if (m_scheduler) m_scheduler->fixedTick(dt);
}

} // namespace ox::gameplay

#endif
