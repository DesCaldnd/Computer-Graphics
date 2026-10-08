#pragma once

#include <oxwald/core/result.hpp>
#include <oxwald/core/serial/value.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <span>
#include <string>
#include <vector>

// Archive encodings of serial::Document.
//
// Binary "OXB1" (little-endian):
//   header   : "OXB1" u16 formatVersion u16 flags u32 dataVersion u32 chunkCount
//   chunk    : char id[4] u32 size u32 crc32(payload) payload
//   META     : varint-string kind
//   STRS     : varint count, varint-strings (type/field names, descriptor texts, enum value names)
//   TYPE     : schema table: varint count; per type: varint nameStr u64 nameHash varint fieldCount
//              {varint nameStr u64 nameHash varint descStr}
//   DATA     : varint rootDescStr, root payload
// Object payload = varint typeIndex, varint fieldCount, records {varint slot, varint byteSize, payload}; the
// byte size lets readers skip fields whose descriptor they do not understand. Arrays of fixed-size elements
// are stored as one raw block. Enums are stored as the FNV-1a hash of the value name.
//
// JSON: {"format":"oxb1-json","formatVersion":1,"kind":..,"version":..,"schema":{type:{field:desc}},
//        "rootType":desc,"data":..}. Every object carries "$type" (a schema key). The schema makes the
// JSON -> binary conversion byte-identical; hand-written JSON may omit it (types are then inferred).
namespace ox::serial {

inline constexpr u16 kBinaryFormatVersion = 1;
inline constexpr char kBinaryMagic[4] = {'O', 'X', 'B', '1'};

enum class Format { Binary, Json };

[[nodiscard]] std::vector<std::byte> encodeBinary(const Document& doc);
[[nodiscard]] Result<Document> decodeBinary(std::span<const std::byte> data);
// Partial decode: only the listed fields of the root object are decoded, the others are skipped by their record
// size without being parsed (e.g. reading a save game's header without its world). Empty list = everything.
struct BinaryDecodeOptions {
    std::vector<std::string> rootFields;
};
[[nodiscard]] Result<Document> decodeBinary(std::span<const std::byte> data, const BinaryDecodeOptions& options);
[[nodiscard]] bool isBinaryArchive(std::span<const std::byte> data);

[[nodiscard]] nlohmann::ordered_json encodeJson(const Document& doc);
[[nodiscard]] Result<Document> decodeJson(const nlohmann::ordered_json& json);
[[nodiscard]] std::string toJsonString(const Document& doc, int indent = 2);
[[nodiscard]] Result<Document> parseJsonString(std::string_view text);

// Lossless conversions that need no knowledge of the C++ types.
[[nodiscard]] Result<std::string> binaryToJson(std::span<const std::byte> binary, int indent = 2);
[[nodiscard]] Result<std::vector<std::byte>> jsonToBinary(std::string_view json);

// Detects the encoding from content (OXB1 magic or JSON).
[[nodiscard]] Result<Document> decodeAny(std::span<const std::byte> data);
[[nodiscard]] Result<Document> loadDocument(const std::filesystem::path& path);
// Writes atomically (temp file + rename).
Status saveDocument(const std::filesystem::path& path, const Document& doc, Format format);
// ".json" extension => Json, otherwise Binary.
[[nodiscard]] Format formatForPath(const std::filesystem::path& path);

Result<std::vector<std::byte>> readFileBytes(const std::filesystem::path& path);
Status writeFileAtomic(const std::filesystem::path& path, std::span<const std::byte> bytes);
// Building blocks of writeFileAtomic for writers that stream: `path` plus a suffix unique to this process and call,
// and the rename over `path` (retried on Windows while another process holds the destination open).
[[nodiscard]] std::filesystem::path uniqueTempPath(const std::filesystem::path& path);
Status replaceFile(const std::filesystem::path& tmp, const std::filesystem::path& path);

struct BinaryInfo {
    struct Chunk {
        std::string id;
        u32 offset = 0;
        u32 size = 0;
        u32 crc = 0;
        bool crcValid = false;
    };
    struct Field {
        std::string name;
        u64 nameHash = 0;
        std::string desc;
    };
    struct Type {
        std::string name;
        u64 nameHash = 0;
        std::vector<Field> fields;
    };
    u16 formatVersion = 0;
    u16 flags = 0;
    u32 dataVersion = 0;
    std::string kind;
    usize fileSize = 0;
    std::vector<Chunk> chunks;
    std::vector<std::string> strings;
    std::vector<Type> types;
};

// Parses header, chunk table and schema without decoding DATA (CRC failures are reported, not fatal).
[[nodiscard]] Result<BinaryInfo> inspectBinary(std::span<const std::byte> data);

} // namespace ox::serial
