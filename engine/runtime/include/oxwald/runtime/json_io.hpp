#pragma once

#include <oxwald/core/reflect.hpp>
#include <oxwald/core/result.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>

// Plain, human-editable JSON for reflected settings structs (.oxproj, user settings). Unlike the archive JSON
// ("oxb1-json" with schema and "$type"), this writes bare objects; reading goes through the tolerant reflection
// converter (missing fields keep defaults, unknown fields are ignored, enums by name).
namespace ox::json {

[[nodiscard]] nlohmann::ordered_json toPlain(const void* object, const reflect::TypeInfo& type);
bool fromPlain(const nlohmann::ordered_json& json, void* object, const reflect::TypeInfo& type);

template <class T>
[[nodiscard]] nlohmann::ordered_json toPlain(const T& object) {
    return toPlain(&object, reflect::typeOf<T>());
}
template <class T>
bool fromPlain(const nlohmann::ordered_json& json, T& object) {
    return fromPlain(json, &object, reflect::typeOf<T>());
}

[[nodiscard]] Result<nlohmann::ordered_json> parse(std::string_view text);
[[nodiscard]] Result<nlohmann::ordered_json> loadFile(const std::filesystem::path& path);
Status saveFile(const std::filesystem::path& path, const nlohmann::ordered_json& json);

} // namespace ox::json
