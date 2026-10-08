#include <oxwald/assets/asset_meta.hpp>

#include "internal.hpp"

namespace ox::assets {

namespace {
void stripTypes(nlohmann::ordered_json& j) {
    if (j.is_object()) {
        j.erase("$type");
        for (auto& [k, v] : j.items()) stripTypes(v);
    } else if (j.is_array()) {
        for (auto& v : j) stripTypes(v);
    }
}
} // namespace

nlohmann::ordered_json valueToPlainJson(const serial::Value& value) {
    serial::Document doc;
    doc.root = value;
    auto j = serial::encodeJson(doc);
    nlohmann::ordered_json data = j["data"];
    stripTypes(data);
    return data;
}

Result<serial::Value> plainJsonToValue(const nlohmann::ordered_json& json) {
    auto doc = serial::decodeJson(json);
    if (!doc) return doc.error();
    return std::move(doc->root);
}

std::filesystem::path metaPathFor(const std::filesystem::path& sourcePath) {
    auto p = sourcePath;
    p += kMetaExtension;
    return p;
}

std::string metaToJson(const AssetMeta& meta) {
    nlohmann::ordered_json j;
    j["formatVersion"] = meta.formatVersion;
    j["uuid"] = meta.uuid.toString();
    j["importer"] = meta.importer;
    j["importerVersion"] = meta.importerVersion;
    j["settings"] = meta.settings.is_object() ? meta.settings : nlohmann::ordered_json::object();
    return j.dump(2) + "\n";
}

Result<AssetMeta> readMeta(const std::filesystem::path& metaPath) {
    auto bytes = detail::readFile(metaPath);
    if (!bytes) return bytes.error();
    auto j = nlohmann::ordered_json::parse(detail::asString(*bytes), nullptr, false);
    if (j.is_discarded() || !j.is_object()) return makeError("{}: invalid JSON", metaPath.string());
    AssetMeta meta;
    meta.formatVersion = j.value("formatVersion", kMetaFormatVersion);
    auto uuid = Uuid::parse(j.value("uuid", std::string{}));
    if (!uuid || uuid->isNil()) return makeError("{}: missing or invalid uuid", metaPath.string());
    meta.uuid = *uuid;
    meta.importer = j.value("importer", std::string{});
    meta.importerVersion = j.value("importerVersion", 0u);
    if (auto it = j.find("settings"); it != j.end() && it->is_object()) meta.settings = *it;
    return meta;
}

Status writeMeta(const std::filesystem::path& metaPath, const AssetMeta& meta) {
    return detail::writeFile(metaPath, detail::asBytes(metaToJson(meta)));
}

} // namespace ox::assets
