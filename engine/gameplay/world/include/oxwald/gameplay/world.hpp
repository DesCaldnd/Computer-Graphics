#pragma once

// World integration of the gameplay module: ECS components + systems for terrain, vegetation, sky, time of day,
// water, wind/weather, buoyancy and chunk streaming, plus the WorldRenderData service (renderer contract).
//
//   ox::registerGameplayTypes();
//   ox::registerWorldGameplayTypes();
//   ox::addGameplaySystems(scheduler, services);
//   ox::gameplay::addWorldSystems(scheduler, services);   // after addGameplaySystems, before scheduler.attach
//   scheduler.attach(world, services);

#include <oxwald/gameplay/world/components.hpp>
#include <oxwald/gameplay/world/render_data.hpp>
#include <oxwald/gameplay/world/runtime.hpp>
#include <oxwald/scene/system.hpp>

#include <string_view>

namespace ox::gameplay {

// System names (SystemScheduler::find / setEnabled). Phase / order / mode:
namespace systems {
inline constexpr std::string_view kWorldLifecycle = "Gameplay.World.Lifecycle";     // PreUpdate   -990 always
inline constexpr std::string_view kWorldStreaming = "Gameplay.World.Streaming";     // PreUpdate   -500 play
inline constexpr std::string_view kWorldTerrain = "Gameplay.World.Terrain";         // PreUpdate   -400 always
inline constexpr std::string_view kWorldEnvironment = "Gameplay.World.Environment"; // PreUpdate   -300 always
inline constexpr std::string_view kWorldBuoyancy = "Gameplay.World.Buoyancy";       // FixedUpdate  -10 play
inline constexpr std::string_view kWorldVegetation = "Gameplay.World.Vegetation";   // PostUpdate   300 always
inline constexpr std::string_view kWorldExtract = "Gameplay.World.Extract";         // Extract       50 always
} // namespace systems

// Creates (when missing) the WorldRuntime and WorldRenderData services, binds the `world` Lua API (ScriptVM service)
// and adds the world systems (idempotent). Call after addGameplaySystems() and before scheduler.attach().
void addWorldSystems(SystemScheduler& scheduler, Services& services, const WorldSystemsConfig& config = {});

} // namespace ox::gameplay

namespace ox {
// Reflection + ComponentRegistry entries of the world components (idempotent). Requires registerSceneTypes().
void registerWorldGameplayTypes();
} // namespace ox
