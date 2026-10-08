#include <oxwald/assets/asset_meta.hpp>
#include <oxwald/assets/material.hpp>
#include <oxwald/core/serial/archive.hpp>

#include "internal.hpp"

namespace ox::assets {

std::vector<Uuid> MaterialAsset::textureDependencies() const {
    std::vector<Uuid> out;
    for (const Uuid& u : {albedoTexture, normalTexture, ormTexture, emissiveTexture, heightTexture}) {
        if (u.isValid() && std::find(out.begin(), out.end(), u) == out.end()) out.push_back(u);
    }
    return out;
}

std::string materialToJson(const MaterialAsset& material) {
    registerAssetTypes();
    nlohmann::ordered_json j;
    j["oxmat"] = 1;
    const auto fields = valueToPlainJson(serial::toValue(material));
    for (const auto& [k, v] : fields.items()) j[k] = v;
    return j.dump(2) + "\n";
}

std::vector<std::byte> materialToBinary(const MaterialAsset& material) {
    registerAssetTypes();
    serial::Writer w("material", 1);
    w.value("material", material);
    return w.toBinary();
}

Status saveMaterial(const MaterialAsset& material, const std::filesystem::path& path) {
    const std::string ext = detail::toLower(path.extension().string());
    if (ext == ".json" || ext == ".oxmat") return detail::writeFile(path, detail::asBytes(materialToJson(material)));
    return detail::writeFile(path, materialToBinary(material));
}

Result<MaterialAsset> loadMaterial(std::span<const std::byte> data) {
    registerAssetTypes();
    MaterialAsset m;
    if (serial::isBinaryArchive(data)) {
        auto doc = serial::decodeBinary(data);
        if (!doc) return doc.error();
        const serial::Value* v = doc->root.find("material");
        if (!v || !serial::fromValue(*v, m)) return makeError("binary material has no 'material' object");
        return m;
    }
    auto j = nlohmann::ordered_json::parse(detail::asString(data), nullptr, false);
    if (j.is_discarded() || !j.is_object()) return makeError("material: invalid JSON");
    if (j.contains("format") && j.contains("data")) { // core JSON archive
        auto doc = serial::decodeJson(j);
        if (!doc) return doc.error();
        const serial::Value* v = doc->root.find("material");
        if (!serial::fromValue(v ? *v : doc->root, m)) return makeError("material: incompatible archive");
        return m;
    }
    j.erase("oxmat");
    auto v = plainJsonToValue(j);
    if (!v) return v.error();
    if (!serial::fromValue(*v, m)) return makeError("material: incompatible JSON");
    return m;
}

Result<MaterialAsset> loadMaterialFile(const std::filesystem::path& path) {
    auto bytes = detail::readFile(path);
    if (!bytes) return bytes.error();
    return loadMaterial(*bytes);
}

} // namespace ox::assets
