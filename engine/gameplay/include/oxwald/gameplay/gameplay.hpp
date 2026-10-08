#pragma once

// Gameplay module: ECS components + systems binding physics, animation, splines, audio, AI, networking and
// scripting to the scene World. See docs/dev/modules/gameplay.md.
//
//   ox::registerSceneTypes();
//   ox::registerGameplayTypes();
//   ox::Services services;                         // optional: PhysicsWorld, ScriptVM, AudioEngine, DebugDraw,
//   ox::SystemScheduler scheduler;                  //           providers (GameplayAssetRegistry), ...
//   ox::addGameplaySystems(scheduler, services);    // creates missing services + runtimes, adds the systems
//   scheduler.attach(world, services);
//   scheduler.setPlaying(true);                     // simulation (physics, scripts, AI, followers) runs in play mode
//   scheduler.tick(world, services, dt);

#include <oxwald/gameplay/ai.hpp>
#include <oxwald/gameplay/animation.hpp>
#include <oxwald/gameplay/audio.hpp>
#include <oxwald/gameplay/common.hpp>
#include <oxwald/gameplay/coroutines.hpp>
#include <oxwald/gameplay/events.hpp>
#include <oxwald/gameplay/net.hpp>
#include <oxwald/gameplay/physics.hpp>
#include <oxwald/gameplay/providers.hpp>
#include <oxwald/gameplay/script.hpp>
#include <oxwald/gameplay/spline.hpp>
#include <oxwald/scene/system.hpp>
#if defined(OX_GAMEPLAY_HAS_WORLD)
#include <oxwald/gameplay/world.hpp>
#endif
#if defined(OX_GAMEPLAY_HAS_ASSETS)
#include <oxwald/gameplay/asset_providers.hpp>
#endif

#include <string_view>

namespace ox::gameplay {

struct GameplayConfig {
    bool physics = true;
    bool animation = true;
    bool splines = true;
    bool audio = true; // uses the audio::AudioEngine service when one is registered
    bool ai = true;
    bool networking = true;
    bool scripting = true;
    bool coroutines = true; // uses the CoroutineScheduler service when one is registered (async module)
    bool world = true;      // world module components/systems (OX_GAMEPLAY_HAS_WORLD builds only)
    bool prediction = true; // client-side prediction with input replay for PredictedCharacter entities

    // Services created when missing.
    physics::PhysicsWorldDesc physicsWorld;
    script::ScriptVMConfig scriptVM;
    bool createEventBus = true;
    // Tick the CoroutineScheduler (PreUpdate + after each physics step). Disable when the engine loop does it.
    bool tickCoroutines = true;
    // Add scene's TransformSystem when the scheduler has none.
    bool addTransformSystem = true;
#if defined(OX_GAMEPLAY_HAS_WORLD)
    WorldSystemsConfig worldSystems; // `physics` is and-ed with `physics` above
#endif
};

// System names (SystemScheduler::find / setEnabled).
namespace systems {
inline constexpr std::string_view kLifecycle = "Gameplay.Lifecycle";
inline constexpr std::string_view kAssetHotReload = "Gameplay.Assets.HotReload";
inline constexpr std::string_view kNetPre = "Gameplay.Net.Poll";
inline constexpr std::string_view kScriptPre = "Gameplay.Script.PreUpdate";
inline constexpr std::string_view kCoroutines = "Gameplay.Coroutines";
inline constexpr std::string_view kScriptFixed = "Gameplay.Script.FixedUpdate";
inline constexpr std::string_view kNetPredict = "Gameplay.Net.Predict";
inline constexpr std::string_view kPerception = "Gameplay.AI.Perception";
inline constexpr std::string_view kBehaviorTrees = "Gameplay.AI.BehaviorTrees";
inline constexpr std::string_view kNavigation = "Gameplay.AI.Navigation";
inline constexpr std::string_view kPhysicsStep = "Gameplay.Physics.Step";
inline constexpr std::string_view kCoroutinesFixed = "Gameplay.Coroutines.Fixed";
inline constexpr std::string_view kScriptUpdate = "Gameplay.Script.Update";
inline constexpr std::string_view kSplineFollowers = "Gameplay.Spline.Followers";
inline constexpr std::string_view kAnimation = "Gameplay.Animation";
inline constexpr std::string_view kPhysicsInterpolate = "Gameplay.Physics.Interpolate";
inline constexpr std::string_view kAudio = "Gameplay.Audio";
inline constexpr std::string_view kNetPost = "Gameplay.Net.Send";
inline constexpr std::string_view kDebugDraw = "Gameplay.DebugDraw";
} // namespace systems

} // namespace ox::gameplay

namespace ox {

// Reflection + ComponentRegistry entries for every gameplay component (idempotent). Requires registerSceneTypes().
void registerGameplayTypes();

// Creates the missing services (PhysicsWorld, ScriptVM, EventBus) and the runtimes (PhysicsRuntime,
// AnimationRuntime, SplineRuntime, AudioRuntime, AIRuntime, NetworkRuntime, ScriptRuntime, CoroutineRuntime) in
// `services`, then adds the gameplay systems to `scheduler`. Call before scheduler.attach().
void addGameplaySystems(SystemScheduler& scheduler, Services& services, const gameplay::GameplayConfig& config = {});

} // namespace ox
