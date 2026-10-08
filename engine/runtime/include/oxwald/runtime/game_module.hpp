#pragma once

// Game modules: native (C++) project code that any host (OxwaldPlayer, OxwaldEditor, tools) picks up without the
// host knowing the project. A game library registers a factory at static-initialisation time and is linked into the
// host with WHOLE_ARCHIVE (see samples/OxwaldShowcase/CMakeLists.txt):
//
//   OX_GAME_MODULE("Showcase", ShowcaseModule)   // in one .cpp of the game library
//
// Registered modules are opt-in per project: Engine::init adds a module only when the project's module toggles name
// it explicitly (`"modules": {"Showcase": true}` in the .oxproj), after the built-in modules and the host's own.

#include <oxwald/runtime/engine.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ox {

using GameModuleFactory = std::function<std::unique_ptr<IEngineModule>()>;

struct GameModuleEntry {
    std::string name;
    GameModuleFactory factory;
};

bool registerGameModule(std::string name, GameModuleFactory factory);
[[nodiscard]] const std::vector<GameModuleEntry>& registeredGameModules();

} // namespace ox

#define OX_GAME_MODULE_CONCAT_(a, b) a##b
#define OX_GAME_MODULE_CONCAT(a, b) OX_GAME_MODULE_CONCAT_(a, b)
#define OX_GAME_MODULE(name, Type)                                                                                     \
    [[maybe_unused]] static const bool OX_GAME_MODULE_CONCAT(oxGameModuleRegistered_, __LINE__) =                     \
        ::ox::registerGameModule(name, [] { return std::unique_ptr<::ox::IEngineModule>(std::make_unique<Type>()); })
