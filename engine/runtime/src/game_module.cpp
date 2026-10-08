#include <oxwald/runtime/game_module.hpp>

namespace ox {

namespace {
std::vector<GameModuleEntry>& entries() {
    static std::vector<GameModuleEntry> e; // function-local: safe from static-initialisation order
    return e;
}
} // namespace

bool registerGameModule(std::string name, GameModuleFactory factory) {
    entries().push_back({std::move(name), std::move(factory)});
    return true;
}

const std::vector<GameModuleEntry>& registeredGameModules() { return entries(); }

} // namespace ox
