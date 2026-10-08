#include <oxwald/core/serial/format.hpp>
#include <oxwald/runtime/json_io.hpp>

namespace ox::json {

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

nlohmann::ordered_json toPlain(const void* object, const reflect::TypeInfo& type) {
    serial::Document doc;
    doc.root = serial::toValue(object, type);
    nlohmann::ordered_json j = serial::encodeJson(doc)["data"];
    stripTypes(j);
    return j;
}

bool fromPlain(const nlohmann::ordered_json& json, void* object, const reflect::TypeInfo& type) {
    if (!json.is_object()) return false;
    auto doc = serial::decodeJson(json);
    if (!doc) return false;
    return serial::fromValue(doc->root, object, type);
}

Result<nlohmann::ordered_json> parse(std::string_view text) {
    auto j = nlohmann::ordered_json::parse(text, nullptr, false, true);
    if (j.is_discarded()) return makeError("invalid JSON");
    return j;
}

Result<nlohmann::ordered_json> loadFile(const std::filesystem::path& path) {
    auto bytes = serial::readFileBytes(path);
    if (!bytes) return bytes.error();
    auto j = parse(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
    if (!j) return makeError("{}: {}", path.string(), j.error().message);
    return j;
}

Status saveFile(const std::filesystem::path& path, const nlohmann::ordered_json& json) {
    const std::string text = json.dump(2) + "\n";
    return serial::writeFileAtomic(path, std::as_bytes(std::span(text.data(), text.size())));
}

} // namespace ox::json
