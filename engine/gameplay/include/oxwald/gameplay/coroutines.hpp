#pragma once

// Entity-owned C++20 coroutines (async module). Only available when the async module is configured
// (OX_GAMEPLAY_HAS_ASYNC=1).
#if defined(OX_GAMEPLAY_HAS_ASYNC)

#include <oxwald/async/async.hpp>
#include <oxwald/core/services.hpp>
#include <oxwald/gameplay/common.hpp>
#include <oxwald/scene/world.hpp>

#include <entt/signal/sigh.hpp>

#include <string>
#include <vector>

namespace ox::gameplay {

// Owner id of an entity's coroutines (CoroutineScheduler::cancelOwner). Runtime-only, never persisted.
[[nodiscard]] inline u64 coroutineOwner(const Entity& e) { return toRuntimeId(e); }

// Unity-style helpers: the coroutine is cancelled (unwound, destructors run) when the entity is destroyed.
// Use the CoroutineScheduler registered in `services`; invalid handle when there is none.
CoroutineHandle startCoroutine(Services& services, Entity e, Task<> task, std::string name = {});
void stopAllCoroutines(Services& services, Entity e);

// Hooks the CoroutineScheduler (service) to the world: cancels an entity's coroutines as soon as it is scheduled
// for destruction (before its components are freed), cancels all world-owned coroutines when play mode stops,
// and optionally ticks the scheduler (GameplayConfig::tickCoroutines).
class CoroutineRuntime {
public:
    CoroutineRuntime() = default;
    ~CoroutineRuntime();
    CoroutineRuntime(const CoroutineRuntime&) = delete;
    CoroutineRuntime& operator=(const CoroutineRuntime&) = delete;

    [[nodiscard]] CoroutineScheduler* scheduler() const { return m_scheduler; }

    void attach(World& world, Services& services);
    void detach();
    void syncPlayState(bool playing);
    void tick(f32 dt, u64 frame);
    void fixedTick(f32 dt);

private:
    void onPendingDestroy(entt::registry& r, entt::entity e);
    void cancelWorldOwners();

    World* m_world = nullptr;
    CoroutineScheduler* m_scheduler = nullptr;
    std::vector<entt::scoped_connection> m_connections;
    bool m_playing = false;
};

} // namespace ox::gameplay

#endif
