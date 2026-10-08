#pragma once

#include <oxwald/core/types.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace ox::script {

enum class ScriptPropertyType : u8 { Float, Int, Bool, String, Vec2, Vec3, Vec4, Color };

// Plain C++ value exchanged with scripts (properties, event payloads from C++). Color is stored as vec4.
using ScriptValue = std::variant<std::monostate, bool, i64, f64, std::string, glm::vec2, glm::vec3, glm::vec4>;

// One entry of a script's `properties = { ... }` declaration; what the editor shows in the inspector.
struct ScriptPropertyDesc {
    std::string name;
    ScriptPropertyType type = ScriptPropertyType::Float;
    ScriptValue defaultValue;
    std::optional<f64> min;
    std::optional<f64> max;
    std::string tooltip;
    i32 order = 0; // display order; ties sorted by name
};

std::string_view toString(ScriptPropertyType type);
std::optional<ScriptPropertyType> parsePropertyType(std::string_view name);
ScriptValue defaultValueFor(ScriptPropertyType type);
// Converts `value` to the property's type (int <-> float, clamps to min/max). nullopt if incompatible.
std::optional<ScriptValue> coerceProperty(const ScriptPropertyDesc& desc, const ScriptValue& value);
std::string toDebugString(const ScriptValue& value);

} // namespace ox::script
