#include "schema.hpp"

#include <oxwald/core/serial/format.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <limits>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace ox::serial {
namespace {

using Json = nlohmann::ordered_json;

constexpr int kMaxDepth = 512;

// Shortest decimal that round-trips the float, widened to double, so "0.1f" prints as 0.1 instead of
// 0.10000000149011612. Verified to convert back to the identical float.
double floatForJson(f32 f) {
    char buf[32];
    for (int prec = 6; prec <= 9; ++prec) {
        std::snprintf(buf, sizeof(buf), "%.*g", prec, double(f));
        if (std::strtof(buf, nullptr) == f) break;
    }
    const double d = std::strtod(buf, nullptr);
    return static_cast<f32>(d) == f ? d : double(f);
}

Json floatJson(f64 d, bool isF32) {
    if (std::isnan(d)) return "nan";
    if (std::isinf(d)) return d > 0 ? "inf" : "-inf";
    return isF32 ? floatForJson(static_cast<f32>(d)) : d;
}

std::optional<f64> jsonToDouble(const Json& j) {
    if (j.is_number()) return j.get<f64>();
    if (j.is_string()) {
        const auto& s = j.get_ref<const std::string&>();
        if (s == "nan") return std::numeric_limits<f64>::quiet_NaN();
        if (s == "inf") return std::numeric_limits<f64>::infinity();
        if (s == "-inf") return -std::numeric_limits<f64>::infinity();
    }
    return std::nullopt;
}

int floatCount(Tag t) {
    switch (t) {
    case Tag::Vec2: return 2;
    case Tag::Vec3: return 3;
    case Tag::Vec4:
    case Tag::Quat: return 4;
    case Tag::Mat4: return 16;
    default: return 0;
    }
}

int intCount(Tag t) {
    switch (t) {
    case Tag::IVec2: return 2;
    case Tag::IVec3: return 3;
    case Tag::IVec4: return 4;
    default: return 0;
    }
}

// ---- encode ------------------------------------------------------------------------------------------------

class JsonEncoder {
public:
    JsonEncoder(const detail::SchemaBuilder& schema, const std::vector<std::string>& keys)
        : m_schema(schema), m_keys(keys) {}

    Json value(const Value& v) const {
        switch (v.tag()) {
        case Tag::Null: return nullptr;
        case Tag::Bool: return v.getBool();
        case Tag::I8:
        case Tag::I16:
        case Tag::I32:
        case Tag::I64: return v.getInt();
        case Tag::U8:
        case Tag::U16:
        case Tag::U32:
        case Tag::U64: return v.getUInt();
        case Tag::F32: return floatJson(v.getDouble(), true);
        case Tag::F64: return floatJson(v.getDouble(), false);
        case Tag::String: return v.getString();
        case Tag::Vec2:
        case Tag::Vec3:
        case Tag::Vec4:
        case Tag::Quat:
        case Tag::Mat4: {
            const auto bytes = v.fixedBytes();
            Json arr = Json::array();
            for (int i = 0; i < floatCount(v.tag()); ++i) {
                f32 f;
                std::memcpy(&f, bytes.data() + i * 4, 4);
                arr.push_back(floatJson(f, true));
            }
            return arr;
        }
        case Tag::IVec2:
        case Tag::IVec3:
        case Tag::IVec4: {
            const auto bytes = v.fixedBytes();
            Json arr = Json::array();
            for (int i = 0; i < intCount(v.tag()); ++i) {
                i32 x;
                std::memcpy(&x, bytes.data() + i * 4, 4);
                arr.push_back(x);
            }
            return arr;
        }
        case Tag::Uuid:
        case Tag::EntityRef: return v.getUuid().toString();
        case Tag::Enum:
            if (v.getString().empty()) return v.enumHash(); // name unknown: keep the hash
            return v.getString();
        case Tag::Object: {
            Json obj = Json::object();
            obj["$type"] = m_keys[m_schema.typeOf(v)];
            for (const auto& [k, child] : v.fields()) obj[k] = value(child);
            return obj;
        }
        case Tag::Array: {
            if (v.isPacked() && v.elemDesc()->tag == Tag::U8) return base64Encode(v.packedBytes());
            Json arr = Json::array();
            const usize n = v.size();
            for (usize i = 0; i < n; ++i) arr.push_back(v.isPacked() ? value(v.at(i)) : value(v.items()[i]));
            return arr;
        }
        case Tag::Optional: return v.hasValue() ? value(v.optionalValue()) : Json(nullptr);
        case Tag::Map: {
            Json obj = Json::object();
            for (const auto& [k, child] : v.fields()) obj[k] = value(child);
            return obj;
        }
        }
        return nullptr;
    }

private:
    const detail::SchemaBuilder& m_schema;
    const std::vector<std::string>& m_keys;
};

// ---- decode ------------------------------------------------------------------------------------------------

struct JsonSchemaType {
    std::string name;
    std::vector<std::pair<std::string, DescPtr>> fields;
    [[nodiscard]] DescPtr find(std::string_view k) const {
        for (const auto& [n, d] : fields) {
            if (n == k) return d;
        }
        return nullptr;
    }
};

std::string stripSchemaKey(const std::string& key) {
    if (!key.empty() && key[0] == '@') return {};
    const auto hash = key.rfind('#');
    if (hash != std::string::npos && hash + 1 < key.size() &&
        key.find_first_not_of("0123456789", hash + 1) == std::string::npos) {
        return key.substr(0, hash);
    }
    return key;
}

class JsonDecoder {
public:
    std::unordered_map<std::string, JsonSchemaType> schema;

    Result<Value> value(const Json& j, const DescPtr& desc, int depth) const {
        if (depth > kMaxDepth) return makeError("JSON nesting too deep");
        if (!desc) return infer(j, depth);
        const Tag tag = desc->tag;
        switch (tag) {
        case Tag::Null:
            if (!j.is_null()) return mismatch(j, desc);
            return Value{};
        case Tag::Bool:
            if (!j.is_boolean()) return mismatch(j, desc);
            return Value::makeBool(j.get<bool>());
        case Tag::I8:
        case Tag::I16:
        case Tag::I32:
        case Tag::I64:
            if (j.is_number_integer() || j.is_number_unsigned()) return Value::makeInt(j.get<i64>(), tag);
            if (j.is_number_float()) return Value::makeInt(static_cast<i64>(j.get<f64>()), tag);
            return mismatch(j, desc);
        case Tag::U8:
        case Tag::U16:
        case Tag::U32:
        case Tag::U64:
            if (j.is_number_unsigned() || j.is_number_integer()) return Value::makeUInt(j.get<u64>(), tag);
            if (j.is_number_float()) return Value::makeUInt(static_cast<u64>(j.get<f64>()), tag);
            return mismatch(j, desc);
        case Tag::F32: {
            auto d = jsonToDouble(j);
            if (!d) return mismatch(j, desc);
            return Value::makeF32(static_cast<f32>(*d));
        }
        case Tag::F64: {
            auto d = jsonToDouble(j);
            if (!d) return mismatch(j, desc);
            return Value::makeF64(*d);
        }
        case Tag::String:
            if (!j.is_string()) return mismatch(j, desc);
            return Value::makeString(j.get<std::string>());
        case Tag::Vec2:
        case Tag::Vec3:
        case Tag::Vec4:
        case Tag::Quat:
        case Tag::Mat4: {
            const int n = floatCount(tag);
            if (!j.is_array() || int(j.size()) != n) return mismatch(j, desc);
            f32 data[16];
            for (int i = 0; i < n; ++i) {
                auto d = jsonToDouble(j[usize(i)]);
                if (!d) return mismatch(j, desc);
                data[i] = static_cast<f32>(*d);
            }
            return Value::fromFixedBytes(tag, reinterpret_cast<const std::byte*>(data));
        }
        case Tag::IVec2:
        case Tag::IVec3:
        case Tag::IVec4: {
            const int n = intCount(tag);
            if (!j.is_array() || int(j.size()) != n) return mismatch(j, desc);
            i32 data[4];
            for (int i = 0; i < n; ++i) {
                if (!j[usize(i)].is_number()) return mismatch(j, desc);
                data[i] = j[usize(i)].get<i32>();
            }
            return Value::fromFixedBytes(tag, reinterpret_cast<const std::byte*>(data));
        }
        case Tag::Uuid:
        case Tag::EntityRef: {
            if (!j.is_string()) return mismatch(j, desc);
            auto id = Uuid::parse(j.get_ref<const std::string&>());
            if (!id) return makeError("JSON: malformed UUID '{}'", j.get<std::string>());
            return tag == Tag::Uuid ? Value::makeUuid(*id) : Value::makeEntityRef(*id);
        }
        case Tag::Enum:
            if (j.is_string()) return Value::makeEnum(j.get<std::string>());
            if (j.is_number_unsigned() || j.is_number_integer()) return Value::makeEnumHash(j.get<u64>());
            return mismatch(j, desc);
        case Tag::Object: return object(j, depth);
        case Tag::Array: {
            if (desc->elem->tag == Tag::U8 && j.is_string()) {
                auto bytes = base64Decode(j.get_ref<const std::string&>());
                if (!bytes) return makeError("JSON: malformed base64 blob");
                return Value::makeRawArray(desc->elem, std::move(*bytes));
            }
            if (!j.is_array()) return mismatch(j, desc);
            Value arr = Value::makeArray(desc->elem);
            arr.reserve(j.size());
            for (const auto& item : j) {
                auto v = value(item, desc->elem, depth + 1);
                if (!v) return v;
                arr.push(std::move(*v));
            }
            return arr;
        }
        case Tag::Optional: {
            if (j.is_null()) return Value::makeOptional(desc->elem);
            auto v = value(j, desc->elem, depth + 1);
            if (!v) return v;
            return Value::makeOptional(desc->elem, std::move(*v));
        }
        case Tag::Map: {
            if (!j.is_object()) return mismatch(j, desc);
            Value map = Value::makeMap(desc->elem);
            for (const auto& [k, item] : j.items()) {
                auto v = value(item, desc->elem, depth + 1);
                if (!v) return v;
                map.fields().emplace_back(k, std::move(*v));
            }
            return map;
        }
        }
        return mismatch(j, desc);
    }

private:
    static Error mismatch(const Json& j, const DescPtr& desc) {
        return makeError("JSON: expected {} but found {}", desc->str(), j.type_name());
    }

    Result<Value> object(const Json& j, int depth) const {
        if (!j.is_object()) return makeError("JSON: expected object but found {}", j.type_name());
        const JsonSchemaType* type = nullptr;
        std::string typeName;
        if (auto it = j.find("$type"); it != j.end()) {
            if (!it->is_string()) return makeError("JSON: $type must be a string");
            const auto& key = it->get_ref<const std::string&>();
            if (auto st = schema.find(key); st != schema.end()) {
                type = &st->second;
                typeName = st->second.name;
            } else {
                typeName = stripSchemaKey(key);
            }
        }
        Value obj = Value::makeObject(std::move(typeName));
        for (const auto& [k, item] : j.items()) {
            if (k == "$type") continue;
            auto v = value(item, type ? type->find(k) : nullptr, depth + 1);
            if (!v) return makeError("{}.{}", k, v.error().message);
            obj.fields().emplace_back(k, std::move(*v));
        }
        return obj;
    }

    // Schema-less JSON (hand written): pick natural types.
    Result<Value> infer(const Json& j, int depth) const {
        switch (j.type()) {
        case Json::value_t::null: return Value{};
        case Json::value_t::boolean: return Value::makeBool(j.get<bool>());
        case Json::value_t::number_integer: return Value::makeInt(j.get<i64>());
        case Json::value_t::number_unsigned: {
            const u64 u = j.get<u64>();
            if (u <= u64(std::numeric_limits<i64>::max())) return Value::makeInt(i64(u));
            return Value::makeUInt(u);
        }
        case Json::value_t::number_float: return Value::makeF64(j.get<f64>());
        case Json::value_t::string: return Value::makeString(j.get<std::string>());
        case Json::value_t::object: return object(j, depth);
        case Json::value_t::array: {
            std::vector<Value> items;
            items.reserve(j.size());
            bool allNumbers = !j.empty();
            bool anyFloat = false;
            for (const auto& item : j) {
                auto v = infer(item, depth + 1);
                if (!v) return v;
                allNumbers = allNumbers && v->isNumber();
                anyFloat = anyFloat || v->tag() == Tag::F64;
                items.push_back(std::move(*v));
            }
            if (allNumbers) {
                Value arr = Value::makeArray(TypeDesc::of(anyFloat ? Tag::F64 : Tag::I64));
                for (const auto& v : items) {
                    arr.push(anyFloat ? Value::makeF64(v.getDouble()) : Value::makeInt(v.getInt()));
                }
                return arr;
            }
            Value arr = Value::makeArray(items.empty() ? nullptr : items.front().desc());
            for (auto& v : items) {
                if (v.tag() != arr.elemDesc()->tag) return makeError("JSON: heterogeneous array cannot be typed");
                arr.items().push_back(std::move(v));
            }
            return arr;
        }
        default: return makeError("JSON: unsupported value");
        }
    }
};

} // namespace

Json encodeJson(const Document& doc) {
    detail::SchemaBuilder schema;
    schema.collect(doc);
    const auto keys = schema.schemaKeys();

    Json j = Json::object();
    j["format"] = "oxb1-json";
    j["formatVersion"] = kBinaryFormatVersion;
    j["kind"] = doc.kind;
    j["version"] = doc.version;
    Json types = Json::object();
    for (usize i = 0; i < schema.types.size(); ++i) {
        Json fields = Json::object();
        for (const auto& [name, desc] : schema.types[i].fields) fields[name] = desc->str();
        types[keys[i]] = std::move(fields);
    }
    j["schema"] = std::move(types);
    j["rootType"] = doc.root.desc()->str();
    j["data"] = JsonEncoder(schema, keys).value(doc.root);
    return j;
}

Result<Document> decodeJson(const Json& j) {
    JsonDecoder decoder;
    Document doc;
    const Json* data = &j;
    DescPtr rootDesc;
    // A bare object (no envelope) is accepted as the root of a schema-less document.
    if (j.is_object() && j.contains("data") && j.contains("format")) {
        if (!j["format"].is_string() || j["format"].get<std::string>() != "oxb1-json") {
            return makeError("JSON archive: unknown format");
        }
        doc.kind = j.value("kind", std::string{});
        doc.version = j.value("version", 0u);
        if (auto it = j.find("schema"); it != j.end() && it->is_object()) {
            for (const auto& [key, fields] : it->items()) {
                JsonSchemaType t;
                t.name = stripSchemaKey(key);
                if (!fields.is_object()) return makeError("JSON archive: bad schema entry '{}'", key);
                for (const auto& [fname, fdesc] : fields.items()) {
                    auto d = fdesc.is_string() ? TypeDesc::parse(fdesc.get<std::string>()) : nullptr;
                    if (!d) return makeError("JSON archive: bad type descriptor for {}.{}", key, fname);
                    t.fields.emplace_back(fname, std::move(d));
                }
                decoder.schema.emplace(key, std::move(t));
            }
        }
        if (auto it = j.find("rootType"); it != j.end() && it->is_string()) {
            rootDesc = TypeDesc::parse(it->get<std::string>());
            if (!rootDesc) return makeError("JSON archive: bad rootType");
        }
        data = &j["data"];
    }
    auto root = decoder.value(*data, rootDesc, 0);
    if (!root) return root.error();
    doc.root = std::move(*root);
    return doc;
}

std::string toJsonString(const Document& doc, int indent) { return encodeJson(doc).dump(indent); }

Result<Document> parseJsonString(std::string_view text) {
    Json j = Json::parse(text.begin(), text.end(), nullptr, false);
    if (j.is_discarded()) return makeError("JSON parse error");
    return decodeJson(j);
}

Result<std::string> binaryToJson(std::span<const std::byte> binary, int indent) {
    auto doc = decodeBinary(binary);
    if (!doc) return doc.error();
    return toJsonString(*doc, indent);
}

Result<std::vector<std::byte>> jsonToBinary(std::string_view json) {
    auto doc = parseJsonString(json);
    if (!doc) return doc.error();
    return encodeBinary(*doc);
}

Result<Document> decodeAny(std::span<const std::byte> data) {
    if (isBinaryArchive(data)) return decodeBinary(data);
    return parseJsonString(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
}

Format formatForPath(const std::filesystem::path& path) {
    return path.extension() == ".json" ? Format::Json : Format::Binary;
}

Result<std::vector<std::byte>> readFileBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return makeError("cannot open '{}'", path.string());
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    in.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<usize>(size));
    if (size > 0 && !in.read(reinterpret_cast<char*>(bytes.data()), size)) {
        return makeError("cannot read '{}'", path.string());
    }
    return bytes;
}

std::filesystem::path uniqueTempPath(const std::filesystem::path& path) {
    static std::atomic<u64> counter{0};
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = int(getpid());
#endif
    auto tmp = path;
    tmp += std::format(".tmp{}_{}", pid, counter.fetch_add(1, std::memory_order_relaxed));
    return tmp;
}

Status replaceFile(const std::filesystem::path& tmp, const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
#if defined(_WIN32)
    // Windows refuses to replace a file somebody has open (another process reading the same cache entry, an
    // indexer): that passes within milliseconds.
    for (int attempt = 0; ec && attempt < 100 && std::filesystem::exists(path); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::filesystem::rename(tmp, path, ec);
    }
#endif
    if (ec) return makeError("cannot replace '{}': {}", path.string(), ec.message());
    return {};
}

Status writeFileAtomic(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    // Not a fixed "<path>.tmp": two writers of one file (processes sharing an asset cache) would write through each
    // other, and the one renaming first would publish a file the other is still truncating and filling.
    const auto tmp = uniqueTempPath(path);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return makeError("cannot write '{}'", tmp.string());
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) return makeError("write failed for '{}'", tmp.string());
    }
    if (auto st = replaceFile(tmp, path); !st) {
        std::filesystem::remove(tmp, ec);
        return st;
    }
    return {};
}

Result<Document> loadDocument(const std::filesystem::path& path) {
    auto bytes = readFileBytes(path);
    if (!bytes) return bytes.error();
    auto doc = decodeAny(*bytes);
    if (!doc) return makeError("{}: {}", path.string(), doc.error().message);
    return doc;
}

Status saveDocument(const std::filesystem::path& path, const Document& doc, Format format) {
    if (format == Format::Binary) return writeFileAtomic(path, encodeBinary(doc));
    const std::string text = toJsonString(doc, 2) + "\n";
    return writeFileAtomic(path, std::as_bytes(std::span(text.data(), text.size())));
}

} // namespace ox::serial
