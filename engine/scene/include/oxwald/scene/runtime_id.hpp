#pragma once

#include <oxwald/core/types.hpp>

#include <entt/entity/entity.hpp>

// Entity <-> u64 for runtime-only references: physics body user data, coroutine owners
// (CoroutineScheduler::cancelOwner), blackboard entity ids, ... Encodes the entt handle (index + version) with a
// tag bit so 0 is never a valid id and stale ids are detected by World::valid(). Never persisted: use EntityRef
// (UUID) for anything that is saved. One helper for every module (gameplay::toRuntimeId/coroutineOwner and the
// runtime's world-unload/destroy paths) so owner ids always match.
namespace ox {

[[nodiscard]] constexpr u64 entityRuntimeId(entt::entity e) {
    return e == entt::null ? 0 : (u64{1} << 32) | static_cast<u64>(entt::to_integral(e));
}
[[nodiscard]] constexpr entt::entity entityFromRuntimeId(u64 id) {
    return (id >> 32) == 1 ? static_cast<entt::entity>(static_cast<u32>(id)) : entt::entity{entt::null};
}

} // namespace ox
