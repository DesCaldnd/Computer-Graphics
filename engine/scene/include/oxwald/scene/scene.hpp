#pragma once

#include <oxwald/scene/component_registry.hpp>
#include <oxwald/scene/components.hpp>
#include <oxwald/scene/entity_ref.hpp>
#include <oxwald/scene/world.hpp>

namespace ox {

// Registers reflection + ComponentRegistry entries for every scene type (EntityRef leaf, core components).
// Call once at startup (idempotent). Other modules follow the same pattern with their own registerXxxTypes().
void registerSceneTypes();

} // namespace ox
