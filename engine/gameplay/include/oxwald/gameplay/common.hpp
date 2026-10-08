#pragma once

#include <oxwald/core/types.hpp>
#include <oxwald/scene/world.hpp>

#include <entt/entity/entity.hpp>

namespace ox::gameplay {

// Entity <-> u64 used as physics body user data, blackboard entity ids and similar runtime-only references.
// Encodes the entt handle (index + version) so stale ids are detected by World::valid(). Never persisted:
// use EntityRef (UUID) for anything that is saved.
[[nodiscard]] constexpr u64 toRuntimeId(entt::entity e) {
    return e == entt::null ? 0 : (u64{1} << 32) | static_cast<u64>(entt::to_integral(e));
}
[[nodiscard]] inline u64 toRuntimeId(const Entity& e) { return e.valid() ? toRuntimeId(e.handle()) : 0; }
[[nodiscard]] constexpr entt::entity fromRuntimeId(u64 id) {
    return (id >> 32) == 1 ? static_cast<entt::entity>(static_cast<u32>(id)) : entt::entity{entt::null};
}
// Invalid Entity when the id is 0 or the entity no longer exists in `world`.
[[nodiscard]] inline Entity entityFromRuntimeId(World& world, u64 id) {
    const entt::entity e = fromRuntimeId(id);
    return e != entt::null && world.valid(e) ? world.wrap(e) : Entity{};
}

// Added by the network runtime on clients to replicated entities that are driven by the server: physics creates
// their bodies as kinematic, so the local simulation does not fight the replicated transform.
struct NetworkProxyTag {};

// Tracks edit <-> play transitions of the scheduler (SystemContext::playing) for the always-running systems.
class PlayStateTracker {
public:
    // Returns +1 when play mode started, -1 when it stopped, 0 otherwise.
    i32 update(bool playing) {
        if (playing == m_playing) return 0;
        m_playing = playing;
        return playing ? 1 : -1;
    }
    [[nodiscard]] bool playing() const { return m_playing; }
    void reset() { m_playing = false; }

private:
    bool m_playing = false;
};

} // namespace ox::gameplay
