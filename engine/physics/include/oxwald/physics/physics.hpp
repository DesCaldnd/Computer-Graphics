#pragma once

// Convenience umbrella header for the physics module.
#include <oxwald/physics/interpolation.hpp>
#include <oxwald/physics/physics_world.hpp>

namespace ox::physics {

// Jolt version + build configuration string, e.g. "Jolt 5.5.0: Single precision ARM 64-bit ...".
const char* backendInfo();

} // namespace ox::physics
