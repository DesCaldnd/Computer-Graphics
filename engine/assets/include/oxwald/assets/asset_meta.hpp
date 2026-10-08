#pragma once

// Sidecar "<source>.meta" (JSON):
//   { "formatVersion": 1, "uuid": "8-4-4-4-12", "importer": "texture", "importerVersion": 3,
//     "settings": { ...importer settings, enums as names... } }

#include <oxwald/core/reflect.hpp>
#include <oxwald/core/result.hpp>
#include <oxwald/core/serial/convert.hpp>
#include <oxwald/core/serial/format.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

namespace ox::assets {

inline constexpr u32 kMetaFormatVersion = 1;
inline constexpr std::string_view kMetaExtension = ".meta";

struct AssetMeta {
    u32 formatVersion = kMetaFormatVersion;
    Uuid uuid;
    std::string importer;
    u32 importerVersion = 0;
    nlohmann::ordered_json settings = nlohmann::ordered_json::object();
};

[[nodiscard]] Result<AssetMeta> readMeta(const std::filesystem::path& metaPath);
Status writeMeta(const std::filesystem::path& metaPath, const AssetMeta& meta);
[[nodiscard]] std::string metaToJson(const AssetMeta& meta);
[[nodiscard]] std::filesystem::path metaPathFor(const std::filesystem::path& sourcePath);

// Reflected value <-> plain JSON (field names, enums as names, vectors as arrays, no "$type").
[[nodiscard]] nlohmann::ordered_json valueToPlainJson(const serial::Value& value);
[[nodiscard]] Result<serial::Value> plainJsonToValue(const nlohmann::ordered_json& json);

template <class T>
[[nodiscard]] nlohmann::ordered_json toSettingsJson(const T& settings) {
    return valueToPlainJson(serial::toValue(settings));
}
// Missing fields keep their defaults; unknown fields are ignored.
template <class T>
bool fromSettingsJson(const nlohmann::ordered_json& json, T& settings) {
    if (!json.is_object()) return false;
    auto v = plainJsonToValue(json);
    return v && serial::fromValue(*v, settings);
}

} // namespace ox::assets
