#include <oxwald/script/script_value.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace ox::script {

std::string_view toString(ScriptPropertyType type) {
    switch (type) {
    case ScriptPropertyType::Float: return "float";
    case ScriptPropertyType::Int: return "int";
    case ScriptPropertyType::Bool: return "bool";
    case ScriptPropertyType::String: return "string";
    case ScriptPropertyType::Vec2: return "vec2";
    case ScriptPropertyType::Vec3: return "vec3";
    case ScriptPropertyType::Vec4: return "vec4";
    case ScriptPropertyType::Color: return "color";
    }
    return "?";
}

std::optional<ScriptPropertyType> parsePropertyType(std::string_view name) {
    static constexpr std::pair<std::string_view, ScriptPropertyType> kNames[] = {
        {"float", ScriptPropertyType::Float},   {"number", ScriptPropertyType::Float},
        {"int", ScriptPropertyType::Int},       {"integer", ScriptPropertyType::Int},
        {"bool", ScriptPropertyType::Bool},     {"boolean", ScriptPropertyType::Bool},
        {"string", ScriptPropertyType::String}, {"vec2", ScriptPropertyType::Vec2},
        {"vec3", ScriptPropertyType::Vec3},     {"vec4", ScriptPropertyType::Vec4},
        {"color", ScriptPropertyType::Color},
    };
    for (auto& [n, t] : kNames) {
        if (n == name) {
            return t;
        }
    }
    return std::nullopt;
}

ScriptValue defaultValueFor(ScriptPropertyType type) {
    switch (type) {
    case ScriptPropertyType::Float: return 0.0;
    case ScriptPropertyType::Int: return i64{0};
    case ScriptPropertyType::Bool: return false;
    case ScriptPropertyType::String: return std::string{};
    case ScriptPropertyType::Vec2: return glm::vec2(0.f);
    case ScriptPropertyType::Vec3: return glm::vec3(0.f);
    case ScriptPropertyType::Vec4: return glm::vec4(0.f);
    case ScriptPropertyType::Color: return glm::vec4(1.f);
    }
    return {};
}

std::optional<ScriptValue> coerceProperty(const ScriptPropertyDesc& desc, const ScriptValue& value) {
    auto clampNum = [&](f64 v) {
        if (desc.min) v = std::max(v, *desc.min);
        if (desc.max) v = std::min(v, *desc.max);
        return v;
    };
    auto number = [&]() -> std::optional<f64> {
        if (auto* d = std::get_if<f64>(&value)) return *d;
        if (auto* i = std::get_if<i64>(&value)) return static_cast<f64>(*i);
        return std::nullopt;
    };
    switch (desc.type) {
    case ScriptPropertyType::Float:
        if (auto n = number()) return ScriptValue{clampNum(*n)};
        return std::nullopt;
    case ScriptPropertyType::Int:
        if (auto n = number()) return ScriptValue{static_cast<i64>(std::llround(clampNum(*n)))};
        return std::nullopt;
    case ScriptPropertyType::Bool:
        if (auto* b = std::get_if<bool>(&value)) return ScriptValue{*b};
        return std::nullopt;
    case ScriptPropertyType::String:
        if (auto* s = std::get_if<std::string>(&value)) return ScriptValue{*s};
        return std::nullopt;
    case ScriptPropertyType::Vec2:
        if (auto* v = std::get_if<glm::vec2>(&value)) return ScriptValue{*v};
        return std::nullopt;
    case ScriptPropertyType::Vec3:
        if (auto* v = std::get_if<glm::vec3>(&value)) return ScriptValue{*v};
        return std::nullopt;
    case ScriptPropertyType::Vec4:
    case ScriptPropertyType::Color:
        if (auto* v = std::get_if<glm::vec4>(&value)) return ScriptValue{*v};
        if (auto* v3 = std::get_if<glm::vec3>(&value); v3 && desc.type == ScriptPropertyType::Color)
            return ScriptValue{glm::vec4(*v3, 1.f)};
        return std::nullopt;
    }
    return std::nullopt;
}

std::string toDebugString(const ScriptValue& value) {
    return std::visit(
        [](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::monostate>) return "nil";
            else if constexpr (std::is_same_v<T, bool>) return v ? "true" : "false";
            else if constexpr (std::is_same_v<T, std::string>) return '"' + v + '"';
            else if constexpr (std::is_same_v<T, glm::vec2>) return std::format("vec2({}, {})", v.x, v.y);
            else if constexpr (std::is_same_v<T, glm::vec3>) return std::format("vec3({}, {}, {})", v.x, v.y, v.z);
            else if constexpr (std::is_same_v<T, glm::vec4>)
                return std::format("vec4({}, {}, {}, {})", v.x, v.y, v.z, v.w);
            else return std::format("{}", v);
        },
        value);
}

} // namespace ox::script
