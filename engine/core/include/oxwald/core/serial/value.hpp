#pragma once

#include <oxwald/core/types.hpp>
#include <oxwald/core/uuid.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A typed, self-describing value tree. Both archive backends (binary OXB1 and JSON) are encodings of this
// tree, which is what makes binary <-> JSON conversion lossless without knowing any C++ types.
namespace ox::serial {

// Wire tags. Values are part of the file format: append only, never renumber.
enum class Tag : u8 {
    Null = 0,
    Bool = 1,
    I8 = 2,
    I16 = 3,
    I32 = 4,
    I64 = 5,
    U8 = 6,
    U16 = 7,
    U32 = 8,
    U64 = 9,
    F32 = 10,
    F64 = 11,
    String = 12,
    Vec2 = 13,
    Vec3 = 14,
    Vec4 = 15,
    IVec2 = 16,
    IVec3 = 17,
    IVec4 = 18,
    Quat = 19,
    Mat4 = 20,
    Uuid = 21,
    EntityRef = 22, // UUID of an entity; remapped when scenes/prefabs are instantiated
    Enum = 23,      // stored by name hash (binary) / name (JSON)
    Object = 24,
    Array = 25,
    Optional = 26,
    Map = 27, // string keys
};

[[nodiscard]] std::string_view tagName(Tag tag);
[[nodiscard]] std::optional<Tag> tagFromName(std::string_view name);
// Byte size of the fixed-size encoding of a tag, 0 for variable-size tags (String, Enum, containers).
[[nodiscard]] usize fixedSize(Tag tag);
[[nodiscard]] constexpr bool isIntegerTag(Tag t) { return t >= Tag::I8 && t <= Tag::U64; }
[[nodiscard]] constexpr bool isSignedTag(Tag t) { return t >= Tag::I8 && t <= Tag::I64; }
[[nodiscard]] constexpr bool isFloatTag(Tag t) { return t == Tag::F32 || t == Tag::F64; }
[[nodiscard]] constexpr bool isNumberTag(Tag t) { return isIntegerTag(t) || isFloatTag(t); }
[[nodiscard]] constexpr bool isContainerTag(Tag t) {
    return t == Tag::Object || t == Tag::Array || t == Tag::Optional || t == Tag::Map;
}

struct TypeDesc;
using DescPtr = std::shared_ptr<const TypeDesc>;

// Type descriptor: a tag plus an element descriptor for Array/Optional/Map. Text form: "f32", "array<vec3>",
// "map<optional<string>>", "object", "enum".
struct TypeDesc {
    Tag tag = Tag::Null;
    DescPtr elem;

    [[nodiscard]] static DescPtr of(Tag tag);
    [[nodiscard]] static DescPtr arrayOf(DescPtr elem);
    [[nodiscard]] static DescPtr optionalOf(DescPtr elem);
    [[nodiscard]] static DescPtr mapOf(DescPtr elem);
    [[nodiscard]] static DescPtr parse(std::string_view text); // nullptr when malformed/unknown
    [[nodiscard]] std::string str() const;
};
[[nodiscard]] bool sameDesc(const DescPtr& a, const DescPtr& b);

class Value {
public:
    using Field = std::pair<std::string, Value>;

    Value() = default;

    // -- construction ------------------------------------------------------------------------------------
    [[nodiscard]] static Value null() { return {}; }
    [[nodiscard]] static Value makeBool(bool v);
    [[nodiscard]] static Value makeInt(i64 v, Tag tag = Tag::I64);
    [[nodiscard]] static Value makeUInt(u64 v, Tag tag = Tag::U64);
    [[nodiscard]] static Value makeF32(f32 v);
    [[nodiscard]] static Value makeF64(f64 v);
    [[nodiscard]] static Value makeString(std::string v);
    [[nodiscard]] static Value makeVec2(glm::vec2 v);
    [[nodiscard]] static Value makeVec3(glm::vec3 v);
    [[nodiscard]] static Value makeVec4(glm::vec4 v);
    [[nodiscard]] static Value makeIVec2(glm::ivec2 v);
    [[nodiscard]] static Value makeIVec3(glm::ivec3 v);
    [[nodiscard]] static Value makeIVec4(glm::ivec4 v);
    [[nodiscard]] static Value makeQuat(glm::quat v);
    [[nodiscard]] static Value makeMat4(const glm::mat4& v);
    [[nodiscard]] static Value makeUuid(Uuid v);
    [[nodiscard]] static Value makeEntityRef(Uuid v);
    [[nodiscard]] static Value makeEnum(std::string name);
    [[nodiscard]] static Value makeEnumHash(u64 nameHash, std::string name = {});
    [[nodiscard]] static Value makeObject(std::string typeName = {});
    // elem may be null: the element type is then taken from the first pushed value.
    [[nodiscard]] static Value makeArray(DescPtr elem = nullptr);
    [[nodiscard]] static Value makeOptional(DescPtr elem, std::optional<Value> value = std::nullopt);
    [[nodiscard]] static Value makeMap(DescPtr elem = nullptr);
    // Builds a value from its fixed-size little-endian encoding (see fixedSize()).
    [[nodiscard]] static Value fromFixedBytes(Tag tag, const std::byte* data);
    // Array of fixed-size elements from a packed block (count * fixedSize(elem->tag) bytes).
    [[nodiscard]] static Value makeRawArray(DescPtr elem, std::vector<std::byte> bytes);

    // -- inspection --------------------------------------------------------------------------------------
    [[nodiscard]] Tag tag() const { return m_tag; }
    [[nodiscard]] DescPtr desc() const;
    [[nodiscard]] const DescPtr& elemDesc() const { return m_elem; }
    [[nodiscard]] bool isNull() const { return m_tag == Tag::Null; }
    [[nodiscard]] bool isNumber() const { return isNumberTag(m_tag); }
    [[nodiscard]] bool isObject() const { return m_tag == Tag::Object; }
    [[nodiscard]] bool isArray() const { return m_tag == Tag::Array; }
    [[nodiscard]] bool isMap() const { return m_tag == Tag::Map; }
    [[nodiscard]] bool isOptional() const { return m_tag == Tag::Optional; }
    [[nodiscard]] bool isString() const { return m_tag == Tag::String; }

    // Scalar getters convert between numeric tags (and bool); they return 0/false for non-numeric values.
    [[nodiscard]] bool getBool() const;
    [[nodiscard]] i64 getInt() const;
    [[nodiscard]] u64 getUInt() const;
    [[nodiscard]] f64 getDouble() const;
    // String value, enum name, or object type name.
    [[nodiscard]] const std::string& getString() const { return m_str; }
    [[nodiscard]] const std::string& typeName() const { return m_str; }
    void setTypeName(std::string name) { m_str = std::move(name); }
    [[nodiscard]] u64 enumHash() const;
    // Vector-like getters accept VecN/IVecN/Quat and arrays of numbers (missing components are 0).
    [[nodiscard]] glm::vec4 getVec() const;
    [[nodiscard]] glm::ivec4 getIVec() const;
    [[nodiscard]] glm::quat getQuat() const;
    [[nodiscard]] glm::mat4 getMat4() const;
    [[nodiscard]] Uuid getUuid() const; // Uuid, EntityRef, or a parseable String
    // Fixed-size little-endian encoding of a scalar/vector value (empty for variable-size tags).
    [[nodiscard]] std::span<const std::byte> fixedBytes() const;

    // -- containers --------------------------------------------------------------------------------------
    // Element count for Array/Map/Object/Optional(0 or 1).
    [[nodiscard]] usize size() const;
    [[nodiscard]] bool empty() const { return size() == 0; }
    // Array element by index (materialised for packed arrays).
    [[nodiscard]] Value at(usize index) const;
    // Arrays of fixed-size elements are always stored packed ("raw block"); others store items.
    [[nodiscard]] bool isPacked() const { return m_tag == Tag::Array && m_packed; }
    [[nodiscard]] std::span<const std::byte> packedBytes() const { return m_bytes; }
    [[nodiscard]] const std::vector<Value>& items() const { return m_items; }
    [[nodiscard]] std::vector<Value>& items() { return m_items; }
    void push(Value v);
    void reserve(usize n);

    [[nodiscard]] const std::vector<Field>& fields() const { return m_fields; }
    [[nodiscard]] std::vector<Field>& fields() { return m_fields; }
    [[nodiscard]] const Value* find(std::string_view key) const;
    [[nodiscard]] Value* find(std::string_view key);
    // Object/Map: replaces an existing entry or appends.
    Value& set(std::string_view key, Value v);
    bool erase(std::string_view key);

    [[nodiscard]] bool hasValue() const { return m_tag == Tag::Optional && !m_items.empty(); }
    [[nodiscard]] const Value& optionalValue() const { return m_items.front(); }

    // Deep structural equality including tags (bitwise for floats).
    friend bool operator==(const Value& a, const Value& b);

    // Visits every value in the tree (pre-order) allowing mutation; used e.g. to remap entity references.
    template <class Fn>
    void visitMutable(Fn&& fn) {
        fn(*this);
        for (auto& v : m_items) v.visitMutable(fn);
        for (auto& [k, v] : m_fields) v.visitMutable(fn);
    }

private:
    void setFixed(Tag tag, const void* data, usize size);

    Tag m_tag = Tag::Null;
    bool m_packed = false;
    DescPtr m_elem;
    std::string m_str;
    std::array<std::byte, 16> m_small{};
    std::vector<std::byte> m_bytes; // Mat4 encoding or packed array block
    std::vector<Value> m_items;
    std::vector<Field> m_fields;
};

// A whole archive: content kind (e.g. "scene"), data version (owner-defined schema version) and root.
struct Document {
    std::string kind;
    u32 version = 0;
    Value root = Value::makeObject();

    friend bool operator==(const Document&, const Document&) = default;
};

std::string base64Encode(std::span<const std::byte> data);
std::optional<std::vector<std::byte>> base64Decode(std::string_view text);

} // namespace ox::serial
