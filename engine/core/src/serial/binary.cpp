#include "schema.hpp"

#include <oxwald/core/hash.hpp>
#include <oxwald/core/serial/format.hpp>

#include <algorithm>
#include <cstring>

namespace ox::serial {
namespace {

constexpr usize kHeaderSize = 16;
constexpr usize kChunkHeaderSize = 12;
constexpr int kMaxDepth = 512;

class ByteWriter {
public:
    std::vector<std::byte> buf;

    void bytes(const void* p, usize n) {
        const auto* b = static_cast<const std::byte*>(p);
        buf.insert(buf.end(), b, b + n);
    }
    template <class T>
    void pod(T v) {
        bytes(&v, sizeof(T));
    }
    void varint(u64 v) {
        while (v >= 0x80) {
            buf.push_back(std::byte((v & 0x7F) | 0x80));
            v >>= 7;
        }
        buf.push_back(std::byte(v));
    }
    void string(std::string_view s) {
        varint(s.size());
        bytes(s.data(), s.size());
    }
    usize reserveU32() {
        const usize at = buf.size();
        pod<u32>(0);
        return at;
    }
    void patchU32(usize at, u32 v) { std::memcpy(buf.data() + at, &v, 4); }
};

class ByteReader {
public:
    ByteReader(const std::byte* p, usize n) : m_p(p), m_end(p + n) {}

    [[nodiscard]] bool ok() const { return m_ok; }
    [[nodiscard]] usize remaining() const { return usize(m_end - m_p); }
    [[nodiscard]] const std::byte* pos() const { return m_p; }

    bool bytes(void* out, usize n) {
        if (!m_ok || remaining() < n) return fail();
        std::memcpy(out, m_p, n);
        m_p += n;
        return true;
    }
    const std::byte* take(usize n) {
        if (!m_ok || remaining() < n) {
            fail();
            return nullptr;
        }
        const std::byte* p = m_p;
        m_p += n;
        return p;
    }
    template <class T>
    T pod() {
        T v{};
        bytes(&v, sizeof(T));
        return v;
    }
    u64 varint() {
        u64 v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (!m_ok || m_p >= m_end) {
                fail();
                return 0;
            }
            const u8 b = u8(*m_p++);
            v |= u64(b & 0x7F) << shift;
            if (!(b & 0x80)) return v;
        }
        fail();
        return 0;
    }
    std::string string() {
        const u64 n = varint();
        if (!m_ok || n > remaining()) {
            fail();
            return {};
        }
        std::string s(reinterpret_cast<const char*>(m_p), usize(n));
        m_p += n;
        return s;
    }
    bool fail() {
        m_ok = false;
        return false;
    }

private:
    const std::byte* m_p;
    const std::byte* m_end;
    bool m_ok = true;
};

// ---- encode ------------------------------------------------------------------------------------------------

class Encoder {
public:
    explicit Encoder(const detail::SchemaBuilder& schema) : m_schema(schema) {}

    void value(ByteWriter& w, const TypeDesc& desc, const Value& v) {
        OX_ASSERT(v.tag() == desc.tag, "archive value tag {} does not match declared type {}", tagName(v.tag()),
                  desc.str());
        switch (desc.tag) {
        case Tag::Null: break;
        case Tag::String: w.string(v.getString()); break;
        case Tag::Enum: w.pod<u64>(v.enumHash()); break;
        case Tag::Object: object(w, v); break;
        case Tag::Array: {
            w.varint(v.size());
            if (v.isPacked()) {
                w.bytes(v.packedBytes().data(), v.packedBytes().size());
            } else {
                for (const auto& item : v.items()) value(w, *desc.elem, item);
            }
            break;
        }
        case Tag::Optional:
            w.pod<u8>(v.hasValue() ? 1 : 0);
            if (v.hasValue()) value(w, *desc.elem, v.optionalValue());
            break;
        case Tag::Map:
            w.varint(v.size());
            for (const auto& [k, item] : v.fields()) {
                w.string(k);
                value(w, *desc.elem, item);
            }
            break;
        default: {
            const auto bytes = v.fixedBytes();
            w.bytes(bytes.data(), bytes.size());
            break;
        }
        }
    }

private:
    void object(ByteWriter& w, const Value& v) {
        const u32 typeIndex = m_schema.typeOf(v);
        const auto& type = m_schema.types[typeIndex];
        w.varint(typeIndex);
        w.varint(v.fields().size());
        for (const auto& [k, child] : v.fields()) {
            const u32 slot = type.slots.at(k);
            w.varint(slot);
            const usize sizeAt = w.reserveU32();
            const usize start = w.buf.size();
            value(w, *type.fields[slot].second, child);
            w.patchU32(sizeAt, static_cast<u32>(w.buf.size() - start));
        }
    }

    const detail::SchemaBuilder& m_schema;
};

void writeChunk(ByteWriter& out, const char id[4], const std::vector<std::byte>& payload) {
    out.bytes(id, 4);
    out.pod<u32>(static_cast<u32>(payload.size()));
    out.pod<u32>(crc32(payload));
    out.bytes(payload.data(), payload.size());
}

// ---- decode ------------------------------------------------------------------------------------------------

struct DecodedSchema {
    struct Field {
        std::string name;
        DescPtr desc; // null when the descriptor is not understood by this build
    };
    struct Type {
        std::string name;
        std::vector<Field> fields;
    };
    std::vector<std::string> strings;
    std::unordered_map<u64, u32> stringByHash;
    std::vector<Type> types;
};

class Decoder {
public:
    explicit Decoder(const DecodedSchema& schema, const std::vector<std::string>* rootFields = nullptr)
        : m_schema(schema), m_rootFields(rootFields && !rootFields->empty() ? rootFields : nullptr) {}

    Result<Value> value(ByteReader& r, const TypeDesc& desc, int depth) {
        if (depth > kMaxDepth) return makeError("archive nesting too deep");
        switch (desc.tag) {
        case Tag::Null: return Value{};
        case Tag::String: {
            auto s = r.string();
            if (!r.ok()) return truncated();
            return Value::makeString(std::move(s));
        }
        case Tag::Enum: {
            const u64 h = r.pod<u64>();
            if (!r.ok()) return truncated();
            auto it = m_schema.stringByHash.find(h);
            return Value::makeEnumHash(h, it != m_schema.stringByHash.end() ? m_schema.strings[it->second] : std::string{});
        }
        case Tag::Object: return object(r, depth);
        case Tag::Array: {
            const u64 n = r.varint();
            if (!r.ok()) return truncated();
            const usize elemSize = fixedSize(desc.elem->tag);
            if (elemSize > 0 && desc.elem->tag != Tag::Enum) {
                if (n > r.remaining() / elemSize) return truncated();
                const std::byte* p = r.take(usize(n) * elemSize);
                return Value::makeRawArray(desc.elem, std::vector<std::byte>(p, p + usize(n) * elemSize));
            }
            if (desc.elem->tag != Tag::Null && n > r.remaining()) return truncated(); // elements take >= 1 byte
            Value arr = Value::makeArray(desc.elem);
            arr.reserve(usize(n));
            for (u64 i = 0; i < n; ++i) {
                auto item = value(r, *desc.elem, depth + 1);
                if (!item) return item;
                arr.items().push_back(std::move(*item));
            }
            return arr;
        }
        case Tag::Optional: {
            const u8 has = r.pod<u8>();
            if (!r.ok() || has > 1) return makeError("archive: bad optional flag");
            if (!has) return Value::makeOptional(desc.elem);
            auto inner = value(r, *desc.elem, depth + 1);
            if (!inner) return inner;
            return Value::makeOptional(desc.elem, std::move(*inner));
        }
        case Tag::Map: {
            const u64 n = r.varint();
            if (!r.ok() || n > r.remaining()) return truncated();
            Value map = Value::makeMap(desc.elem);
            map.reserve(usize(n));
            for (u64 i = 0; i < n; ++i) {
                auto key = r.string();
                if (!r.ok()) return truncated();
                auto item = value(r, *desc.elem, depth + 1);
                if (!item) return item;
                map.fields().emplace_back(std::move(key), std::move(*item));
            }
            return map;
        }
        default: {
            const usize n = fixedSize(desc.tag);
            const std::byte* p = r.take(n);
            if (!p) return truncated();
            return Value::fromFixedBytes(desc.tag, p);
        }
        }
    }

private:
    static Error truncated() { return makeError("archive data truncated or malformed"); }

    Result<Value> object(ByteReader& r, int depth) {
        const u64 typeIndex = r.varint();
        const u64 count = r.varint();
        if (!r.ok()) return truncated();
        if (typeIndex >= m_schema.types.size()) return makeError("archive: bad type index {}", typeIndex);
        const auto& type = m_schema.types[usize(typeIndex)];
        if (count > r.remaining()) return truncated();
        Value obj = Value::makeObject(type.name);
        obj.reserve(usize(count));
        for (u64 i = 0; i < count; ++i) {
            const u64 slot = r.varint();
            const u32 size = r.pod<u32>();
            if (!r.ok() || size > r.remaining()) return truncated();
            if (slot >= type.fields.size()) return makeError("archive: bad field slot {} in {}", slot, type.name);
            const auto& field = type.fields[usize(slot)];
            const std::byte* start = r.take(size);
            if (!field.desc) continue; // descriptor from a newer format version: skip by size
            if (depth == 0 && m_rootFields &&
                std::find(m_rootFields->begin(), m_rootFields->end(), field.name) == m_rootFields->end()) {
                continue; // partial decode: not requested
            }
            ByteReader sub(start, size);
            auto v = value(sub, *field.desc, depth + 1);
            if (!v) return v;
            if (sub.remaining() != 0) return makeError("archive: field {}.{} size mismatch", type.name, field.name);
            obj.fields().emplace_back(field.name, std::move(*v));
        }
        return obj;
    }

    const DecodedSchema& m_schema;
    const std::vector<std::string>* m_rootFields = nullptr;
};

struct ParsedChunk {
    char id[4];
    usize offset;
    u32 size;
    u32 crc;
    bool crcValid;
};

struct ParsedHeader {
    u16 formatVersion = 0;
    u16 flags = 0;
    u32 dataVersion = 0;
    std::vector<ParsedChunk> chunks;
};

Result<ParsedHeader> parseContainer(std::span<const std::byte> data) {
    if (!isBinaryArchive(data)) return makeError("not an OXB1 archive");
    if (data.size() < kHeaderSize) return makeError("OXB1 header truncated");
    ByteReader r(data.data() + 4, kHeaderSize - 4);
    ParsedHeader h;
    h.formatVersion = r.pod<u16>();
    h.flags = r.pod<u16>();
    h.dataVersion = r.pod<u32>();
    const u32 chunkCount = r.pod<u32>();
    if (h.formatVersion == 0 || h.formatVersion > kBinaryFormatVersion) {
        return makeError("unsupported OXB1 format version {}", h.formatVersion);
    }
    usize offset = kHeaderSize;
    for (u32 i = 0; i < chunkCount; ++i) {
        if (data.size() - offset < kChunkHeaderSize) return makeError("OXB1 chunk table truncated");
        ParsedChunk c{};
        std::memcpy(c.id, data.data() + offset, 4);
        std::memcpy(&c.size, data.data() + offset + 4, 4);
        std::memcpy(&c.crc, data.data() + offset + 8, 4);
        offset += kChunkHeaderSize;
        if (data.size() - offset < c.size) return makeError("OXB1 chunk '{}' truncated", std::string_view(c.id, 4));
        c.offset = offset;
        c.crcValid = crc32(data.subspan(offset, c.size)) == c.crc;
        offset += c.size;
        h.chunks.push_back(c);
    }
    return h;
}

const ParsedChunk* findChunk(const ParsedHeader& h, const char* id) {
    for (const auto& c : h.chunks) {
        if (std::memcmp(c.id, id, 4) == 0) return &c;
    }
    return nullptr;
}

Result<DecodedSchema> readSchema(std::span<const std::byte> data, const ParsedHeader& h,
                                 std::vector<BinaryInfo::Type>* infoTypes) {
    DecodedSchema schema;
    if (const auto* c = findChunk(h, "STRS")) {
        ByteReader r(data.data() + c->offset, c->size);
        const u64 n = r.varint();
        if (!r.ok() || n > c->size) return makeError("OXB1 string table malformed");
        schema.strings.reserve(usize(n));
        for (u64 i = 0; i < n; ++i) {
            schema.strings.push_back(r.string());
            schema.stringByHash.try_emplace(fnv1a64(schema.strings.back()), u32(i));
        }
        if (!r.ok()) return makeError("OXB1 string table malformed");
    }
    auto strAt = [&](u64 i) -> const std::string* { return i < schema.strings.size() ? &schema.strings[usize(i)] : nullptr; };
    if (const auto* c = findChunk(h, "TYPE")) {
        ByteReader r(data.data() + c->offset, c->size);
        const u64 n = r.varint();
        if (!r.ok() || n > c->size) return makeError("OXB1 type table malformed");
        for (u64 i = 0; i < n; ++i) {
            DecodedSchema::Type t;
            BinaryInfo::Type info;
            const std::string* name = strAt(r.varint());
            info.nameHash = r.pod<u64>();
            const u64 fieldCount = r.varint();
            if (!r.ok() || !name || fieldCount > c->size) return makeError("OXB1 type table malformed");
            t.name = *name;
            info.name = *name;
            for (u64 f = 0; f < fieldCount; ++f) {
                const std::string* fname = strAt(r.varint());
                const u64 fhash = r.pod<u64>();
                const std::string* fdesc = strAt(r.varint());
                if (!r.ok() || !fname || !fdesc) return makeError("OXB1 type table malformed");
                t.fields.push_back({*fname, TypeDesc::parse(*fdesc)});
                info.fields.push_back({*fname, fhash, *fdesc});
            }
            schema.types.push_back(std::move(t));
            if (infoTypes) infoTypes->push_back(std::move(info));
        }
    }
    return schema;
}

} // namespace

bool isBinaryArchive(std::span<const std::byte> data) {
    return data.size() >= 4 && std::memcmp(data.data(), kBinaryMagic, 4) == 0;
}

std::vector<std::byte> encodeBinary(const Document& doc) {
    detail::SchemaBuilder schema;
    schema.collect(doc);

    ByteWriter meta;
    meta.string(doc.kind);

    ByteWriter strs;
    strs.varint(schema.strings.size());
    for (const auto& s : schema.strings) strs.string(s);

    ByteWriter types;
    types.varint(schema.types.size());
    for (const auto& t : schema.types) {
        types.varint(schema.str(t.name));
        types.pod<u64>(fnv1a64(t.name));
        types.varint(t.fields.size());
        for (const auto& [name, desc] : t.fields) {
            types.varint(schema.str(name));
            types.pod<u64>(fnv1a64(name));
            types.varint(schema.str(desc->str()));
        }
    }

    ByteWriter data;
    const auto rootDesc = doc.root.desc();
    data.varint(schema.str(rootDesc->str()));
    Encoder(schema).value(data, *rootDesc, doc.root);

    ByteWriter out;
    out.bytes(kBinaryMagic, 4);
    out.pod<u16>(kBinaryFormatVersion);
    out.pod<u16>(0);
    out.pod<u32>(doc.version);
    out.pod<u32>(4);
    writeChunk(out, "META", meta.buf);
    writeChunk(out, "STRS", strs.buf);
    writeChunk(out, "TYPE", types.buf);
    writeChunk(out, "DATA", data.buf);
    return std::move(out.buf);
}

Result<Document> decodeBinary(std::span<const std::byte> data) { return decodeBinary(data, BinaryDecodeOptions{}); }

Result<Document> decodeBinary(std::span<const std::byte> data, const BinaryDecodeOptions& options) {
    auto header = parseContainer(data);
    if (!header) return header.error();
    for (const auto& c : header->chunks) {
        if (!c.crcValid) {
            return makeError("OXB1 chunk '{}' failed CRC check (file corrupted)", std::string_view(c.id, 4));
        }
    }
    auto schema = readSchema(data, *header, nullptr);
    if (!schema) return schema.error();

    Document doc;
    doc.version = header->dataVersion;
    if (const auto* c = findChunk(*header, "META")) {
        ByteReader r(data.data() + c->offset, c->size);
        doc.kind = r.string();
        if (!r.ok()) return makeError("OXB1 META chunk malformed");
    }
    const auto* dc = findChunk(*header, "DATA");
    if (!dc) return makeError("OXB1 archive has no DATA chunk");
    ByteReader r(data.data() + dc->offset, dc->size);
    const u64 rootDescIndex = r.varint();
    if (!r.ok() || rootDescIndex >= schema->strings.size()) return makeError("OXB1 DATA chunk malformed");
    auto rootDesc = TypeDesc::parse(schema->strings[usize(rootDescIndex)]);
    if (!rootDesc) return makeError("OXB1 root type '{}' not supported", schema->strings[usize(rootDescIndex)]);
    auto root = Decoder(*schema, &options.rootFields).value(r, *rootDesc, 0);
    if (!root) return root.error();
    if (r.remaining() != 0) return makeError("OXB1 DATA chunk has trailing bytes");
    doc.root = std::move(*root);
    return doc;
}

Result<BinaryInfo> inspectBinary(std::span<const std::byte> data) {
    auto header = parseContainer(data);
    if (!header) return header.error();
    BinaryInfo info;
    info.formatVersion = header->formatVersion;
    info.flags = header->flags;
    info.dataVersion = header->dataVersion;
    info.fileSize = data.size();
    for (const auto& c : header->chunks) {
        info.chunks.push_back({std::string(c.id, 4), static_cast<u32>(c.offset), c.size, c.crc, c.crcValid});
    }
    if (const auto* c = findChunk(*header, "META"); c && c->crcValid) {
        ByteReader r(data.data() + c->offset, c->size);
        info.kind = r.string();
    }
    auto schema = readSchema(data, *header, &info.types);
    if (schema) info.strings = std::move(schema->strings);
    return info;
}

} // namespace ox::serial
