#include <oxwald/core/assert.hpp>
#include <oxwald/core/hash.hpp>
#include <oxwald/core/serial/value.hpp>

#include <algorithm>
#include <bit>
#include <map>
#include <mutex>

static_assert(std::endian::native == std::endian::little, "OXB1 encoding assumes a little-endian host");

namespace ox::serial {
namespace {

struct TagEntry {
    Tag tag;
    std::string_view name;
    usize size;
};

constexpr TagEntry kTags[] = {
    {Tag::Null, "null", 0},      {Tag::Bool, "bool", 1},      {Tag::I8, "i8", 1},
    {Tag::I16, "i16", 2},        {Tag::I32, "i32", 4},        {Tag::I64, "i64", 8},
    {Tag::U8, "u8", 1},          {Tag::U16, "u16", 2},        {Tag::U32, "u32", 4},
    {Tag::U64, "u64", 8},        {Tag::F32, "f32", 4},        {Tag::F64, "f64", 8},
    {Tag::String, "string", 0},  {Tag::Vec2, "vec2", 8},      {Tag::Vec3, "vec3", 12},
    {Tag::Vec4, "vec4", 16},     {Tag::IVec2, "ivec2", 8},    {Tag::IVec3, "ivec3", 12},
    {Tag::IVec4, "ivec4", 16},   {Tag::Quat, "quat", 16},     {Tag::Mat4, "mat4", 64},
    {Tag::Uuid, "uuid", 16},     {Tag::EntityRef, "entity", 16}, {Tag::Enum, "enum", 0},
    {Tag::Object, "object", 0},  {Tag::Array, "array", 0},    {Tag::Optional, "optional", 0},
    {Tag::Map, "map", 0},
};

const TagEntry* entryFor(Tag tag) {
    const auto i = static_cast<usize>(tag);
    return i < std::size(kTags) ? &kTags[i] : nullptr;
}

template <class T>
T loadAs(const std::byte* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

} // namespace

std::string_view tagName(Tag tag) {
    const auto* e = entryFor(tag);
    return e ? e->name : std::string_view("?");
}

std::optional<Tag> tagFromName(std::string_view name) {
    for (const auto& e : kTags) {
        if (e.name == name) return e.tag;
    }
    return std::nullopt;
}

usize fixedSize(Tag tag) {
    if (tag == Tag::Null) return 0;
    const auto* e = entryFor(tag);
    return e ? e->size : 0;
}

// ---- TypeDesc --------------------------------------------------------------------------------------------

DescPtr TypeDesc::of(Tag tag) {
    static const auto table = [] {
        std::array<DescPtr, std::size(kTags)> t;
        for (usize i = 0; i < t.size(); ++i) {
            t[i] = std::make_shared<TypeDesc>(TypeDesc{static_cast<Tag>(i), nullptr});
        }
        return t;
    }();
    const auto i = static_cast<usize>(tag);
    OX_ASSERT(i < table.size(), "bad tag {}", i);
    return table[i];
}

DescPtr TypeDesc::arrayOf(DescPtr elem) {
    return std::make_shared<TypeDesc>(TypeDesc{Tag::Array, elem ? std::move(elem) : of(Tag::Null)});
}
DescPtr TypeDesc::optionalOf(DescPtr elem) {
    return std::make_shared<TypeDesc>(TypeDesc{Tag::Optional, elem ? std::move(elem) : of(Tag::Null)});
}
DescPtr TypeDesc::mapOf(DescPtr elem) {
    return std::make_shared<TypeDesc>(TypeDesc{Tag::Map, elem ? std::move(elem) : of(Tag::Null)});
}

std::string TypeDesc::str() const {
    std::string s(tagName(tag));
    if (tag == Tag::Array || tag == Tag::Optional || tag == Tag::Map) {
        s += '<';
        s += elem ? elem->str() : std::string("null");
        s += '>';
    }
    return s;
}

DescPtr TypeDesc::parse(std::string_view text) {
    const auto lt = text.find('<');
    if (lt == std::string_view::npos) {
        const auto tag = tagFromName(text);
        if (!tag || *tag == Tag::Array || *tag == Tag::Optional || *tag == Tag::Map) return nullptr;
        return of(*tag);
    }
    if (text.back() != '>') return nullptr;
    const auto tag = tagFromName(text.substr(0, lt));
    if (!tag) return nullptr;
    auto inner = parse(text.substr(lt + 1, text.size() - lt - 2));
    if (!inner) return nullptr;
    switch (*tag) {
    case Tag::Array: return arrayOf(std::move(inner));
    case Tag::Optional: return optionalOf(std::move(inner));
    case Tag::Map: return mapOf(std::move(inner));
    default: return nullptr;
    }
}

bool sameDesc(const DescPtr& a, const DescPtr& b) {
    if (a == b) return true;
    // A null element descriptor (lazily typed, still empty array) is equivalent to "null".
    if (!a || !b) return (a ? a->tag : Tag::Null) == (b ? b->tag : Tag::Null);
    if (a->tag != b->tag) return false;
    if (a->tag == Tag::Array || a->tag == Tag::Optional || a->tag == Tag::Map) return sameDesc(a->elem, b->elem);
    return true;
}

// ---- Value -----------------------------------------------------------------------------------------------

void Value::setFixed(Tag tag, const void* data, usize size) {
    m_tag = tag;
    if (size <= m_small.size()) {
        std::memcpy(m_small.data(), data, size);
    } else {
        m_bytes.resize(size);
        std::memcpy(m_bytes.data(), data, size);
    }
}

Value Value::makeBool(bool v) {
    Value r;
    const u8 b = v ? 1 : 0;
    r.setFixed(Tag::Bool, &b, 1);
    return r;
}

Value Value::makeInt(i64 v, Tag tag) {
    OX_ASSERT(isIntegerTag(tag), "makeInt with non-integer tag");
    Value r;
    // Little-endian truncation keeps the low bytes, which is exactly the narrower two's complement value.
    r.setFixed(tag, &v, fixedSize(tag));
    return r;
}

Value Value::makeUInt(u64 v, Tag tag) {
    OX_ASSERT(isIntegerTag(tag), "makeUInt with non-integer tag");
    Value r;
    r.setFixed(tag, &v, fixedSize(tag));
    return r;
}

Value Value::makeF32(f32 v) {
    Value r;
    r.setFixed(Tag::F32, &v, 4);
    return r;
}
Value Value::makeF64(f64 v) {
    Value r;
    r.setFixed(Tag::F64, &v, 8);
    return r;
}
Value Value::makeString(std::string v) {
    Value r;
    r.m_tag = Tag::String;
    r.m_str = std::move(v);
    return r;
}
Value Value::makeVec2(glm::vec2 v) {
    Value r;
    r.setFixed(Tag::Vec2, &v, 8);
    return r;
}
Value Value::makeVec3(glm::vec3 v) {
    Value r;
    r.setFixed(Tag::Vec3, &v, 12);
    return r;
}
Value Value::makeVec4(glm::vec4 v) {
    Value r;
    r.setFixed(Tag::Vec4, &v, 16);
    return r;
}
Value Value::makeIVec2(glm::ivec2 v) {
    Value r;
    r.setFixed(Tag::IVec2, &v, 8);
    return r;
}
Value Value::makeIVec3(glm::ivec3 v) {
    Value r;
    r.setFixed(Tag::IVec3, &v, 12);
    return r;
}
Value Value::makeIVec4(glm::ivec4 v) {
    Value r;
    r.setFixed(Tag::IVec4, &v, 16);
    return r;
}
Value Value::makeQuat(glm::quat q) {
    const f32 xyzw[4] = {q.x, q.y, q.z, q.w};
    Value r;
    r.setFixed(Tag::Quat, xyzw, 16);
    return r;
}
Value Value::makeMat4(const glm::mat4& m) {
    Value r;
    r.setFixed(Tag::Mat4, &m[0][0], 64);
    return r;
}
Value Value::makeUuid(Uuid v) {
    const u64 words[2] = {v.hi, v.lo};
    Value r;
    r.setFixed(Tag::Uuid, words, 16);
    return r;
}
Value Value::makeEntityRef(Uuid v) {
    Value r = makeUuid(v);
    r.m_tag = Tag::EntityRef;
    return r;
}
Value Value::makeEnum(std::string name) {
    const u64 h = fnv1a64(name);
    return makeEnumHash(h, std::move(name));
}
Value Value::makeEnumHash(u64 nameHash, std::string name) {
    Value r;
    r.setFixed(Tag::Enum, &nameHash, 8);
    r.m_str = std::move(name);
    return r;
}
Value Value::makeObject(std::string typeName) {
    Value r;
    r.m_tag = Tag::Object;
    r.m_str = std::move(typeName);
    return r;
}
Value Value::makeArray(DescPtr elem) {
    Value r;
    r.m_tag = Tag::Array;
    r.m_elem = std::move(elem);
    r.m_packed = r.m_elem && fixedSize(r.m_elem->tag) > 0;
    return r;
}
Value Value::makeOptional(DescPtr elem, std::optional<Value> value) {
    Value r;
    r.m_tag = Tag::Optional;
    r.m_elem = elem ? std::move(elem) : (value ? value->desc() : TypeDesc::of(Tag::Null));
    if (value) r.m_items.push_back(std::move(*value));
    return r;
}
Value Value::makeMap(DescPtr elem) {
    Value r;
    r.m_tag = Tag::Map;
    r.m_elem = std::move(elem);
    return r;
}

Value Value::fromFixedBytes(Tag tag, const std::byte* data) {
    const usize n = fixedSize(tag);
    OX_ASSERT(n > 0 && tag != Tag::Enum, "fromFixedBytes: tag {} has no fixed encoding", tagName(tag));
    Value r;
    r.setFixed(tag, data, n);
    return r;
}

Value Value::makeRawArray(DescPtr elem, std::vector<std::byte> bytes) {
    OX_ASSERT(elem && fixedSize(elem->tag) > 0, "makeRawArray needs a fixed-size element type");
    OX_ASSERT(bytes.size() % fixedSize(elem->tag) == 0, "packed block size mismatch");
    Value r = makeArray(std::move(elem));
    r.m_bytes = std::move(bytes);
    return r;
}

DescPtr Value::desc() const {
    switch (m_tag) {
    case Tag::Array: return TypeDesc::arrayOf(m_elem);
    case Tag::Optional: return TypeDesc::optionalOf(m_elem);
    case Tag::Map: return TypeDesc::mapOf(m_elem);
    default: return TypeDesc::of(m_tag);
    }
}

std::span<const std::byte> Value::fixedBytes() const {
    const usize n = fixedSize(m_tag);
    if (n == 0) return {};
    if (n <= m_small.size()) return {m_small.data(), n};
    return m_bytes;
}

bool Value::getBool() const {
    if (m_tag == Tag::Bool) return m_small[0] != std::byte{0};
    if (isNumber()) return getDouble() != 0.0;
    return false;
}

i64 Value::getInt() const {
    const auto* p = m_small.data();
    switch (m_tag) {
    case Tag::Bool: return m_small[0] != std::byte{0} ? 1 : 0;
    case Tag::I8: return loadAs<i8>(p);
    case Tag::I16: return loadAs<i16>(p);
    case Tag::I32: return loadAs<i32>(p);
    case Tag::I64: return loadAs<i64>(p);
    case Tag::U8: return loadAs<u8>(p);
    case Tag::U16: return loadAs<u16>(p);
    case Tag::U32: return loadAs<u32>(p);
    case Tag::U64: return static_cast<i64>(loadAs<u64>(p));
    case Tag::F32: return static_cast<i64>(loadAs<f32>(p));
    case Tag::F64: return static_cast<i64>(loadAs<f64>(p));
    default: return 0;
    }
}

u64 Value::getUInt() const {
    if (m_tag == Tag::U64) return loadAs<u64>(m_small.data());
    if (m_tag == Tag::F32 || m_tag == Tag::F64) {
        const f64 d = getDouble();
        return d <= 0.0 ? 0 : static_cast<u64>(d);
    }
    return static_cast<u64>(getInt());
}

f64 Value::getDouble() const {
    switch (m_tag) {
    case Tag::F32: return loadAs<f32>(m_small.data());
    case Tag::F64: return loadAs<f64>(m_small.data());
    case Tag::U64: return static_cast<f64>(loadAs<u64>(m_small.data()));
    default: return static_cast<f64>(getInt());
    }
}

u64 Value::enumHash() const {
    if (m_tag == Tag::Enum) return loadAs<u64>(m_small.data());
    if (m_tag == Tag::String) return fnv1a64(m_str);
    return 0;
}

glm::vec4 Value::getVec() const {
    glm::vec4 r(0.0f);
    const auto* p = m_small.data();
    switch (m_tag) {
    case Tag::Vec2: std::memcpy(&r, p, 8); return r;
    case Tag::Vec3: std::memcpy(&r, p, 12); return r;
    case Tag::Vec4:
    case Tag::Quat: std::memcpy(&r, p, 16); return r;
    case Tag::IVec2:
    case Tag::IVec3:
    case Tag::IVec4: return glm::vec4(getIVec());
    case Tag::Array:
        for (usize i = 0; i < std::min<usize>(4, size()); ++i) r[int(i)] = static_cast<f32>(at(i).getDouble());
        return r;
    default:
        if (isNumber()) return glm::vec4(static_cast<f32>(getDouble()));
        return r;
    }
}

glm::ivec4 Value::getIVec() const {
    glm::ivec4 r(0);
    const auto* p = m_small.data();
    switch (m_tag) {
    case Tag::IVec2: std::memcpy(&r, p, 8); return r;
    case Tag::IVec3: std::memcpy(&r, p, 12); return r;
    case Tag::IVec4: std::memcpy(&r, p, 16); return r;
    case Tag::Vec2:
    case Tag::Vec3:
    case Tag::Vec4: return glm::ivec4(getVec());
    case Tag::Array:
        for (usize i = 0; i < std::min<usize>(4, size()); ++i) r[int(i)] = static_cast<i32>(at(i).getInt());
        return r;
    default:
        if (isNumber()) return glm::ivec4(static_cast<i32>(getInt()));
        return r;
    }
}

glm::quat Value::getQuat() const {
    if (m_tag != Tag::Quat && m_tag != Tag::Vec4 && m_tag != Tag::Array) return glm::quat(1, 0, 0, 0);
    const glm::vec4 v = getVec();
    return glm::quat(v.w, v.x, v.y, v.z);
}

glm::mat4 Value::getMat4() const {
    glm::mat4 m(1.0f);
    if (m_tag == Tag::Mat4) {
        std::memcpy(&m[0][0], m_bytes.data(), 64);
    } else if (m_tag == Tag::Array && size() == 16) {
        for (usize i = 0; i < 16; ++i) m[int(i / 4)][int(i % 4)] = static_cast<f32>(at(i).getDouble());
    }
    return m;
}

Uuid Value::getUuid() const {
    if (m_tag == Tag::Uuid || m_tag == Tag::EntityRef) {
        return Uuid{loadAs<u64>(m_small.data()), loadAs<u64>(m_small.data() + 8)};
    }
    if (m_tag == Tag::String) return Uuid::parse(m_str).value_or(Uuid{});
    return {};
}

usize Value::size() const {
    switch (m_tag) {
    case Tag::Array: return m_packed ? m_bytes.size() / fixedSize(m_elem->tag) : m_items.size();
    case Tag::Optional: return m_items.size();
    case Tag::Object:
    case Tag::Map: return m_fields.size();
    default: return 0;
    }
}

Value Value::at(usize index) const {
    OX_ASSERT(m_tag == Tag::Array && index < size(), "Value::at out of range");
    if (m_packed) {
        const usize n = fixedSize(m_elem->tag);
        return fromFixedBytes(m_elem->tag, m_bytes.data() + index * n);
    }
    return m_items[index];
}

void Value::reserve(usize n) {
    if (m_packed) {
        m_bytes.reserve(n * fixedSize(m_elem->tag));
    } else if (m_tag == Tag::Object || m_tag == Tag::Map) {
        m_fields.reserve(n);
    } else {
        m_items.reserve(n);
    }
}

void Value::push(Value v) {
    OX_ASSERT(m_tag == Tag::Array, "push on non-array value");
    if (!m_elem || (m_elem->tag == Tag::Null && size() == 0)) {
        m_elem = v.desc();
        m_packed = fixedSize(m_elem->tag) > 0;
    }
    if (m_packed) {
        OX_ASSERT(v.tag() == m_elem->tag, "packed array of {} cannot hold {}", tagName(m_elem->tag), tagName(v.tag()));
        const auto bytes = v.fixedBytes();
        m_bytes.insert(m_bytes.end(), bytes.begin(), bytes.end());
    } else {
        OX_ASSERT(v.tag() == m_elem->tag, "array of {} cannot hold {}", tagName(m_elem->tag), tagName(v.tag()));
        m_items.push_back(std::move(v));
    }
}

const Value* Value::find(std::string_view key) const {
    for (const auto& [k, v] : m_fields) {
        if (k == key) return &v;
    }
    return nullptr;
}

Value* Value::find(std::string_view key) {
    for (auto& [k, v] : m_fields) {
        if (k == key) return &v;
    }
    return nullptr;
}

Value& Value::set(std::string_view key, Value v) {
    OX_ASSERT(m_tag == Tag::Object || m_tag == Tag::Map, "set on non-object value");
    if (m_tag == Tag::Map && !m_elem) m_elem = v.desc();
    if (Value* existing = find(key)) {
        *existing = std::move(v);
        return *existing;
    }
    m_fields.emplace_back(std::string(key), std::move(v));
    return m_fields.back().second;
}

bool Value::erase(std::string_view key) {
    const auto it = std::find_if(m_fields.begin(), m_fields.end(), [&](const Field& f) { return f.first == key; });
    if (it == m_fields.end()) return false;
    m_fields.erase(it);
    return true;
}

bool operator==(const Value& a, const Value& b) {
    if (a.m_tag != b.m_tag) return false;
    switch (a.m_tag) {
    case Tag::Null: return true;
    case Tag::String: return a.m_str == b.m_str;
    case Tag::Enum: return a.enumHash() == b.enumHash();
    case Tag::Object: return a.m_str == b.m_str && a.m_fields == b.m_fields;
    case Tag::Map: return sameDesc(a.m_elem, b.m_elem) && a.m_fields == b.m_fields;
    case Tag::Optional: return sameDesc(a.m_elem, b.m_elem) && a.m_items == b.m_items;
    case Tag::Array:
        return sameDesc(a.m_elem, b.m_elem) && a.m_packed == b.m_packed && a.m_bytes == b.m_bytes &&
               a.m_items == b.m_items;
    default: {
        const auto x = a.fixedBytes();
        const auto y = b.fixedBytes();
        return x.size() == y.size() && std::memcmp(x.data(), y.data(), x.size()) == 0;
    }
    }
}

// ---- base64 ----------------------------------------------------------------------------------------------

std::string base64Encode(std::span<const std::byte> data) {
    static constexpr char kChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    usize i = 0;
    for (; i + 2 < data.size(); i += 3) {
        const u32 n = (u32(data[i]) << 16) | (u32(data[i + 1]) << 8) | u32(data[i + 2]);
        out += kChars[(n >> 18) & 63];
        out += kChars[(n >> 12) & 63];
        out += kChars[(n >> 6) & 63];
        out += kChars[n & 63];
    }
    if (i < data.size()) {
        u32 n = u32(data[i]) << 16;
        if (i + 1 < data.size()) n |= u32(data[i + 1]) << 8;
        out += kChars[(n >> 18) & 63];
        out += kChars[(n >> 12) & 63];
        out += i + 1 < data.size() ? kChars[(n >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::optional<std::vector<std::byte>> base64Decode(std::string_view text) {
    auto decodeChar = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    if (text.size() % 4 != 0) return std::nullopt;
    std::vector<std::byte> out;
    out.reserve(text.size() / 4 * 3);
    for (usize i = 0; i < text.size(); i += 4) {
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = text[i + usize(k)];
            if (c == '=' && i + 4 == text.size() && k >= 2) {
                v[k] = 0;
                ++pad;
            } else {
                if (pad) return std::nullopt;
                v[k] = decodeChar(c);
                if (v[k] < 0) return std::nullopt;
            }
        }
        const u32 n = (u32(v[0]) << 18) | (u32(v[1]) << 12) | (u32(v[2]) << 6) | u32(v[3]);
        out.push_back(std::byte(n >> 16));
        if (pad < 2) out.push_back(std::byte((n >> 8) & 0xFF));
        if (pad < 1) out.push_back(std::byte(n & 0xFF));
    }
    return out;
}

} // namespace ox::serial
