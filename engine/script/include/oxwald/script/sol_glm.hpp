#pragma once

#include <sol/sol.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <type_traits>

// Must be visible in every translation unit that moves glm values through sol2 (included by script_vm.hpp).
// glm types expose value_type/length()/operator[] which can make sol2 treat them as containers or try to
// "automagically" bind members; they are plain value usertypes.
namespace sol {
template <>
struct is_container<glm::vec2> : std::false_type {};
template <>
struct is_container<glm::vec3> : std::false_type {};
template <>
struct is_container<glm::vec4> : std::false_type {};
template <>
struct is_container<glm::quat> : std::false_type {};
template <>
struct is_container<glm::mat4> : std::false_type {};
template <>
struct is_automagical<glm::vec2> : std::false_type {};
template <>
struct is_automagical<glm::vec3> : std::false_type {};
template <>
struct is_automagical<glm::vec4> : std::false_type {};
template <>
struct is_automagical<glm::quat> : std::false_type {};
template <>
struct is_automagical<glm::mat4> : std::false_type {};
} // namespace sol
