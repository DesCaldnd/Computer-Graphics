#pragma once

#include <oxwald/core/assert.hpp>
#include <oxwald/core/hash.hpp>
#include <oxwald/core/serial/value.hpp>
#include <oxwald/core/types.hpp>
#include <oxwald/core/uuid.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

// Runtime reflection. Every module registers its types explicitly from a `registerXxxTypes()` function called
// from the module's init (static-initialiser registration is unreliable in static libraries because the
// linker drops unreferenced object files). Registration is idempotent.
//
//   OX_REFLECT_TYPE(LightComponent, "Light")
//       .attributes(ox::attr::Category{"Rendering"})
//       .field("color", &LightComponent::color, ox::attr::Color{})
//       .field("intensity", &LightComponent::intensity, ox::attr::Range{0.f, 100000.f});
//   OX_REFLECT_TYPE(LightType, "LightType").value("Point", LightType::Point).value("Spot", LightType::Spot);

namespace ox::attr {
struct DisplayName {
    std::string_view value;
};
struct Tooltip {
    std::string_view value;
};
struct Range {
    double min;
    double max;
};
struct Step {
    double value;
};
struct Color {
    bool hdr = false;
};
struct AssetRef {
    std::string_view type; // asset type name, e.g. "Mesh", "Material", "Texture"
};
struct Hidden {};
struct ReadOnly {};
struct Category {
    std::string_view value;
};
struct SaveGame {};
struct Replicated {};
struct NoSerialize {};
// Previous field name(s): readers accept data written under these names.
struct FormerName {
    std::string_view value;
};
// Free-form key/value for module-specific editor hints (e.g. {"icon", "light"}).
struct Meta {
    std::string_view key;
    std::string_view value;
};
} // namespace ox::attr

namespace ox::reflect {

struct Attributes {
    std::string displayName;
    std::string tooltip;
    std::string category;
    std::optional<double> rangeMin;
    std::optional<double> rangeMax;
    std::optional<double> step;
    std::optional<std::string> assetType; // set => UUID field is an asset reference
    bool color = false;
    bool hdr = false;
    bool hidden = false;
    bool readOnly = false;
    bool saveGame = false;
    bool replicated = false;
    bool noSerialize = false;
    std::vector<std::string> formerNames;
    std::vector<std::pair<std::string, std::string>> meta;

    void apply(const attr::DisplayName& a) { displayName = a.value; }
    void apply(const attr::Tooltip& a) { tooltip = a.value; }
    void apply(const attr::Range& a) {
        rangeMin = a.min;
        rangeMax = a.max;
    }
    void apply(const attr::Step& a) { step = a.value; }
    void apply(const attr::Color& a) {
        color = true;
        hdr = a.hdr;
    }
    void apply(const attr::AssetRef& a) { assetType = std::string(a.type); }
    void apply(const attr::Hidden&) { hidden = true; }
    void apply(const attr::ReadOnly&) { readOnly = true; }
    void apply(const attr::Category& a) { category = a.value; }
    void apply(const attr::SaveGame&) { saveGame = true; }
    void apply(const attr::Replicated&) { replicated = true; }
    void apply(const attr::NoSerialize&) { noSerialize = true; }
    void apply(const attr::FormerName& a) { formerNames.emplace_back(a.value); }
    void apply(const attr::Meta& a) { meta.emplace_back(std::string(a.key), std::string(a.value)); }

    [[nodiscard]] std::string_view getMeta(std::string_view key) const {
        for (const auto& [k, v] : meta) {
            if (k == key) return v;
        }
        return {};
    }
};

enum class Kind : u8 {
    Bool,
    Int,
    UInt,
    Float,
    String,
    Math, // glm vectors, quat, mat4
    Uuid,
    Enum,
    Struct,
    Array,    // std::vector<T>
    Optional, // std::optional<T>
    Map,      // std::map / std::unordered_map with std::string keys
    Custom,   // leaf type with user conversion to/from serial::Value (e.g. scene's EntityRef)
};

struct TypeInfo;

struct FieldInfo {
    std::string name;
    u64 nameHash = 0;
    const TypeInfo* type = nullptr;
    Attributes attributes;
    std::function<void*(void*)> access; // object pointer -> field pointer

    [[nodiscard]] void* get(void* object) const { return access(object); }
    [[nodiscard]] const void* get(const void* object) const { return access(const_cast<void*>(object)); }
    [[nodiscard]] std::string_view displayName() const {
        return attributes.displayName.empty() ? std::string_view(name) : std::string_view(attributes.displayName);
    }
};

struct EnumEntry {
    std::string name;
    i64 value = 0;
    u64 nameHash = 0;
    Attributes attributes;
};

struct TypeInfo {
    std::string name;
    u64 id = 0; // fnv1a64(name)
    usize size = 0;
    usize align = 0;
    Kind kind = Kind::Struct;
    serial::Tag tag = serial::Tag::Object; // wire tag for leaves
    bool registered = false;              // explicitly registered (structs/enums) or built-in
    Attributes attributes;

    // Lifetime (null when the C++ type does not support the operation).
    void (*construct)(void*) = nullptr;
    void (*destruct)(void*) = nullptr;
    void (*copyAssign)(void* dst, const void* src) = nullptr;

    // Struct
    const TypeInfo* base = nullptr;
    std::vector<FieldInfo> fields; // includes base-class fields first

    // Enum
    std::vector<EnumEntry> enumEntries;
    i64 (*getEnum)(const void*) = nullptr;
    void (*setEnum)(void*, i64) = nullptr;

    // Containers (Array / Optional / Map); `element` is the value type.
    const TypeInfo* element = nullptr;
    usize (*containerSize)(const void*) = nullptr;
    void (*containerClear)(void*) = nullptr;
    void (*arrayResize)(void*, usize) = nullptr;
    void* (*arrayAt)(void*, usize) = nullptr;
    bool (*optionalHas)(const void*) = nullptr;
    void* (*optionalEmplace)(void*) = nullptr;
    void* (*optionalGet)(void*) = nullptr;
    void (*mapForEach)(const void*, const std::function<void(std::string_view, const void*)>&) = nullptr;
    void* (*mapInsert)(void*, std::string_view) = nullptr; // inserts default if absent, returns value
    void* (*mapFind)(void*, std::string_view) = nullptr;
    bool (*mapErase)(void*, std::string_view) = nullptr;
    // Contiguous trivially-copyable element storage whose memory layout equals the wire encoding.
    bool packedLayout = false;
    void* (*arrayData)(void*) = nullptr;

    // Custom leaves
    std::function<serial::Value(const void*)> customToValue;
    std::function<bool(void*, const serial::Value&)> customFromValue;

    [[nodiscard]] const FieldInfo* findField(std::string_view fieldName) const;
    // Field by current name or any attr::FormerName.
    [[nodiscard]] const FieldInfo* findFieldOrFormer(std::string_view fieldName) const;
    [[nodiscard]] const EnumEntry* findEnum(std::string_view entryName) const;
    [[nodiscard]] const EnumEntry* findEnumByValue(i64 value) const;
    [[nodiscard]] const EnumEntry* findEnumByHash(u64 nameHash) const;
    [[nodiscard]] bool isLeaf() const { return kind != Kind::Struct && kind != Kind::Array && kind != Kind::Optional && kind != Kind::Map; }
    [[nodiscard]] bool isDerivedFrom(const TypeInfo& other) const;

    // Heap instance (default constructed). destroy() must be used to free it.
    [[nodiscard]] void* create() const;
    void destroy(void* object) const;
};

namespace detail {
template <class T>
struct TypeKey {
    static constexpr char tag = 0;
};
} // namespace detail

class TypeRegistry {
public:
    static TypeRegistry& instance();

    template <class T>
    TypeInfo& getOrCreate();

    [[nodiscard]] const TypeInfo* find(std::string_view name) const;
    [[nodiscard]] const TypeInfo* findById(u64 id) const;
    [[nodiscard]] std::vector<const TypeInfo*> all() const;
    [[nodiscard]] std::vector<const TypeInfo*> registeredStructs() const;

    // Internal: (re)names a type and indexes it by name.
    void setName(TypeInfo& info, std::string_view name);
    std::recursive_mutex& mutex();

private:
    TypeRegistry();
    ~TypeRegistry();
    TypeInfo* findByKey(const void* key) const;
    TypeInfo& insert(const void* key, std::unique_ptr<TypeInfo> info);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

template <class T>
const TypeInfo& typeOf() {
    static TypeInfo* info = &TypeRegistry::instance().getOrCreate<std::remove_cvref_t<T>>();
    return *info;
}

template <class T>
TypeInfo& mutableTypeOf() {
    return const_cast<TypeInfo&>(typeOf<T>());
}

// ---- type traits used to fill TypeInfo -------------------------------------------------------------------

namespace detail {
template <class T>
struct IsVector : std::false_type {};
template <class T, class A>
struct IsVector<std::vector<T, A>> : std::true_type {
    using Elem = T;
};
template <class T>
struct IsOptional : std::false_type {};
template <class T>
struct IsOptional<std::optional<T>> : std::true_type {
    using Elem = T;
};
template <class T>
struct IsStringMap : std::false_type {};
template <class V, class C, class A>
struct IsStringMap<std::map<std::string, V, C, A>> : std::true_type {
    using Elem = V;
    static constexpr bool sorted = true;
};
template <class V, class H, class E, class A>
struct IsStringMap<std::unordered_map<std::string, V, H, E, A>> : std::true_type {
    using Elem = V;
    static constexpr bool sorted = false;
};

template <class T>
constexpr serial::Tag mathTag() {
    if constexpr (std::is_same_v<T, glm::vec2>) return serial::Tag::Vec2;
    else if constexpr (std::is_same_v<T, glm::vec3>) return serial::Tag::Vec3;
    else if constexpr (std::is_same_v<T, glm::vec4>) return serial::Tag::Vec4;
    else if constexpr (std::is_same_v<T, glm::ivec2>) return serial::Tag::IVec2;
    else if constexpr (std::is_same_v<T, glm::ivec3>) return serial::Tag::IVec3;
    else if constexpr (std::is_same_v<T, glm::ivec4>) return serial::Tag::IVec4;
    else if constexpr (std::is_same_v<T, glm::quat>) return serial::Tag::Quat;
    else if constexpr (std::is_same_v<T, glm::mat4>) return serial::Tag::Mat4;
    else return serial::Tag::Null;
}
template <class T>
constexpr bool isMath = mathTag<T>() != serial::Tag::Null;

template <class T>
constexpr serial::Tag integerTag() {
    if constexpr (std::is_signed_v<T>) {
        if constexpr (sizeof(T) == 1) return serial::Tag::I8;
        else if constexpr (sizeof(T) == 2) return serial::Tag::I16;
        else if constexpr (sizeof(T) == 4) return serial::Tag::I32;
        else return serial::Tag::I64;
    } else {
        if constexpr (sizeof(T) == 1) return serial::Tag::U8;
        else if constexpr (sizeof(T) == 2) return serial::Tag::U16;
        else if constexpr (sizeof(T) == 4) return serial::Tag::U32;
        else return serial::Tag::U64;
    }
}

// Element types whose in-memory layout equals the OXB1 fixed encoding (quat is xyzw in glm by default).
template <class T>
constexpr bool hasPackedLayout = (std::is_arithmetic_v<T> && !std::is_same_v<T, bool>) || isMath<T> ||
                                 std::is_same_v<T, Uuid>;

template <class T>
void fillLifetime(TypeInfo& info) {
    info.size = sizeof(T);
    info.align = alignof(T);
    if constexpr (std::is_default_constructible_v<T>) {
        info.construct = [](void* p) { new (p) T(); };
    }
    info.destruct = [](void* p) { static_cast<T*>(p)->~T(); };
    if constexpr (std::is_copy_assignable_v<T>) {
        info.copyAssign = [](void* d, const void* s) { *static_cast<T*>(d) = *static_cast<const T*>(s); };
    }
}

template <class T>
void fillTypeInfo(TypeInfo& info);
} // namespace detail

template <class T>
TypeInfo& TypeRegistry::getOrCreate() {
    std::lock_guard lock(mutex());
    const void* key = &detail::TypeKey<T>::tag;
    if (TypeInfo* existing = findByKey(key)) return *existing;
    TypeInfo& info = insert(key, std::make_unique<TypeInfo>());
    detail::fillTypeInfo<T>(info);
    return info;
}

namespace detail {

std::string defaultTypeName(const std::type_info& ti);

template <class T>
void fillTypeInfo(TypeInfo& info) {
    using serial::Tag;
    fillLifetime<T>(info);
    auto& reg = TypeRegistry::instance();
    auto setBuiltin = [&](Kind kind, Tag tag, std::string_view name) {
        info.kind = kind;
        info.tag = tag;
        info.registered = true;
        reg.setName(info, name);
    };

    if constexpr (std::is_same_v<T, bool>) {
        setBuiltin(Kind::Bool, Tag::Bool, "bool");
    } else if constexpr (std::is_integral_v<T>) {
        constexpr Tag tag = integerTag<T>();
        setBuiltin(std::is_signed_v<T> ? Kind::Int : Kind::UInt, tag, serial::tagName(tag));
    } else if constexpr (std::is_same_v<T, float>) {
        setBuiltin(Kind::Float, Tag::F32, "f32");
    } else if constexpr (std::is_same_v<T, double>) {
        setBuiltin(Kind::Float, Tag::F64, "f64");
    } else if constexpr (std::is_same_v<T, std::string>) {
        setBuiltin(Kind::String, Tag::String, "string");
    } else if constexpr (isMath<T>) {
        setBuiltin(Kind::Math, mathTag<T>(), serial::tagName(mathTag<T>()));
    } else if constexpr (std::is_same_v<T, Uuid>) {
        setBuiltin(Kind::Uuid, Tag::Uuid, "uuid");
    } else if constexpr (std::is_enum_v<T>) {
        info.kind = Kind::Enum;
        info.tag = Tag::Enum;
        info.getEnum = [](const void* p) { return static_cast<i64>(*static_cast<const T*>(p)); };
        info.setEnum = [](void* p, i64 v) { *static_cast<T*>(p) = static_cast<T>(v); };
        reg.setName(info, defaultTypeName(typeid(T)));
    } else if constexpr (IsVector<T>::value) {
        using E = typename IsVector<T>::Elem;
        static_assert(!std::is_same_v<E, bool>, "std::vector<bool> is not reflectable; use std::vector<u8>");
        info.kind = Kind::Array;
        info.tag = Tag::Array;
        info.registered = true;
        info.element = &TypeRegistry::instance().getOrCreate<E>();
        info.containerSize = [](const void* p) { return static_cast<const T*>(p)->size(); };
        info.containerClear = [](void* p) { static_cast<T*>(p)->clear(); };
        info.arrayResize = [](void* p, usize n) { static_cast<T*>(p)->resize(n); };
        info.arrayAt = [](void* p, usize i) -> void* { return &(*static_cast<T*>(p))[i]; };
        if constexpr (hasPackedLayout<E>) {
            info.packedLayout = true;
            info.arrayData = [](void* p) -> void* { return static_cast<T*>(p)->data(); };
        }
        reg.setName(info, "array<" + info.element->name + ">");
    } else if constexpr (IsOptional<T>::value) {
        using E = typename IsOptional<T>::Elem;
        info.kind = Kind::Optional;
        info.tag = Tag::Optional;
        info.registered = true;
        info.element = &TypeRegistry::instance().getOrCreate<E>();
        info.containerSize = [](const void* p) -> usize { return static_cast<const T*>(p)->has_value() ? 1 : 0; };
        info.containerClear = [](void* p) { static_cast<T*>(p)->reset(); };
        info.optionalHas = [](const void* p) { return static_cast<const T*>(p)->has_value(); };
        info.optionalEmplace = [](void* p) -> void* { return &static_cast<T*>(p)->emplace(); };
        info.optionalGet = [](void* p) -> void* {
            auto* o = static_cast<T*>(p);
            return o->has_value() ? &**o : nullptr;
        };
        reg.setName(info, "optional<" + info.element->name + ">");
    } else if constexpr (IsStringMap<T>::value) {
        using E = typename IsStringMap<T>::Elem;
        info.kind = Kind::Map;
        info.tag = Tag::Map;
        info.registered = true;
        info.element = &TypeRegistry::instance().getOrCreate<E>();
        info.containerSize = [](const void* p) { return static_cast<const T*>(p)->size(); };
        info.containerClear = [](void* p) { static_cast<T*>(p)->clear(); };
        info.mapForEach = [](const void* p, const std::function<void(std::string_view, const void*)>& fn) {
            const auto& m = *static_cast<const T*>(p);
            if constexpr (IsStringMap<T>::sorted) {
                for (const auto& [k, v] : m) fn(k, &v);
            } else {
                // Deterministic output regardless of hash order.
                std::vector<const typename T::value_type*> entries;
                entries.reserve(m.size());
                for (const auto& kv : m) entries.push_back(&kv);
                std::sort(entries.begin(), entries.end(), [](auto* a, auto* b) { return a->first < b->first; });
                for (const auto* kv : entries) fn(kv->first, &kv->second);
            }
        };
        info.mapInsert = [](void* p, std::string_view k) -> void* {
            return &(*static_cast<T*>(p))[std::string(k)];
        };
        info.mapFind = [](void* p, std::string_view k) -> void* {
            auto& m = *static_cast<T*>(p);
            auto it = m.find(std::string(k));
            return it == m.end() ? nullptr : &it->second;
        };
        info.mapErase = [](void* p, std::string_view k) { return static_cast<T*>(p)->erase(std::string(k)) > 0; };
        reg.setName(info, "map<" + info.element->name + ">");
    } else if constexpr (std::is_class_v<T>) {
        info.kind = Kind::Struct;
        info.tag = Tag::Object;
        reg.setName(info, defaultTypeName(typeid(T)));
    } else {
        static_assert(sizeof(T) == 0, "type cannot be reflected");
    }
}

template <class T, class M>
std::function<void*(void*)> memberAccessor(M T::*member) {
    return [member](void* obj) -> void* { return &(static_cast<T*>(obj)->*member); };
}

} // namespace detail

// ---- builders --------------------------------------------------------------------------------------------

template <class T>
class TypeBuilder {
    static_assert(std::is_class_v<T>, "TypeBuilder is for structs/classes");

public:
    explicit TypeBuilder(std::string_view name) : m_info(mutableTypeOf<T>()) {
        std::lock_guard lock(TypeRegistry::instance().mutex());
        TypeRegistry::instance().setName(m_info, name);
        // Re-registration replaces the field list so registerXxxTypes() may run more than once.
        m_info.fields.clear();
        m_info.attributes = {};
        m_info.base = nullptr;
        m_info.registered = true;
        m_info.kind = Kind::Struct;
        m_info.tag = serial::Tag::Object;
    }

    template <class M, class... A>
    TypeBuilder& field(std::string_view name, M T::*member, A&&... attrs) {
        static_assert(!std::is_function_v<M>, "field() takes data members");
        OX_ASSERT(m_info.findField(name) == nullptr, "duplicate field '{}' in {}", name, m_info.name);
        FieldInfo f;
        f.name = std::string(name);
        f.nameHash = fnv1a64(name);
        f.type = &typeOf<M>();
        f.access = detail::memberAccessor(member);
        (f.attributes.apply(attrs), ...);
        m_info.fields.push_back(std::move(f));
        return *this;
    }

    // Inherit the fields of an already registered base class.
    template <class B>
    TypeBuilder& base() {
        static_assert(std::is_base_of_v<B, T>, "not a base class");
        const TypeInfo& b = typeOf<B>();
        m_info.base = &b;
        std::vector<FieldInfo> inherited;
        for (const auto& bf : b.fields) {
            FieldInfo f = bf;
            f.access = [acc = bf.access](void* obj) -> void* { return acc(static_cast<B*>(static_cast<T*>(obj))); };
            inherited.push_back(std::move(f));
        }
        m_info.fields.insert(m_info.fields.begin(), inherited.begin(), inherited.end());
        return *this;
    }

    template <class... A>
    TypeBuilder& attributes(A&&... attrs) {
        (m_info.attributes.apply(attrs), ...);
        return *this;
    }

    [[nodiscard]] TypeInfo& info() { return m_info; }

private:
    TypeInfo& m_info;
};

template <class E>
class EnumBuilder {
    static_assert(std::is_enum_v<E>);

public:
    explicit EnumBuilder(std::string_view name) : m_info(mutableTypeOf<E>()) {
        std::lock_guard lock(TypeRegistry::instance().mutex());
        TypeRegistry::instance().setName(m_info, name);
        m_info.enumEntries.clear();
        m_info.registered = true;
    }

    template <class... A>
    EnumBuilder& value(std::string_view name, E v, A&&... attrs) {
        EnumEntry e;
        e.name = std::string(name);
        e.value = static_cast<i64>(v);
        e.nameHash = fnv1a64(name);
        (e.attributes.apply(attrs), ...);
        m_info.enumEntries.push_back(std::move(e));
        return *this;
    }

    template <class... A>
    EnumBuilder& attributes(A&&... attrs) {
        (m_info.attributes.apply(attrs), ...);
        return *this;
    }

    [[nodiscard]] TypeInfo& info() { return m_info; }

private:
    TypeInfo& m_info;
};

template <class T>
using BuilderFor = std::conditional_t<std::is_enum_v<T>, EnumBuilder<T>, TypeBuilder<T>>;

template <class T>
BuilderFor<T> registerType(std::string_view name) {
    return BuilderFor<T>(name);
}

// Registers a leaf type with its own conversion to/from the archive value tree (e.g. EntityRef -> Tag::EntityRef).
template <class T>
TypeInfo& registerCustomLeaf(std::string_view name, serial::Tag tag, std::function<serial::Value(const T&)> toValue,
                             std::function<bool(T&, const serial::Value&)> fromValue) {
    TypeInfo& info = mutableTypeOf<T>();
    std::lock_guard lock(TypeRegistry::instance().mutex());
    TypeRegistry::instance().setName(info, name);
    info.kind = Kind::Custom;
    info.tag = tag;
    info.registered = true;
    info.fields.clear();
    info.customToValue = [fn = std::move(toValue)](const void* p) { return fn(*static_cast<const T*>(p)); };
    info.customFromValue = [fn = std::move(fromValue)](void* p, const serial::Value& v) {
        return fn(*static_cast<T*>(p), v);
    };
    return info;
}

// Registers reflection for core's own value types ("ox.Transform", "ox.AABB", "ox.Sphere"). Idempotent.
void registerCoreReflection();

// ---- ValueRef: type-erased reference to a reflected value ------------------------------------------------

struct ValueRef {
    void* ptr = nullptr;
    const TypeInfo* type = nullptr;

    ValueRef() = default;
    ValueRef(void* p, const TypeInfo* t) : ptr(p), type(t) {}
    template <class T>
    static ValueRef of(T& object) {
        return {&object, &typeOf<T>()};
    }

    [[nodiscard]] bool valid() const { return ptr != nullptr && type != nullptr; }
    explicit operator bool() const { return valid(); }

    template <class T>
    [[nodiscard]] T* as() const {
        return valid() && type == &typeOf<T>() ? static_cast<T*>(ptr) : nullptr;
    }

    // One path step: struct field (or former name), vector/quat component (x y z w / r g b a), array index,
    // map key, or "value" of an optional.
    [[nodiscard]] ValueRef child(std::string_view name) const;
    [[nodiscard]] ValueRef index(usize i) const;

    // Generic access through the archive value tree (numeric conversions are tolerant).
    [[nodiscard]] serial::Value get() const;
    bool set(const serial::Value& value) const;

    template <class T>
    [[nodiscard]] std::optional<T> getAs() const;
    template <class T>
    bool setAs(const T& value) const;
};

// Resolves "transform.position.x", "materials[2]", "tags.someKey" relative to root. Invalid ref on failure.
[[nodiscard]] ValueRef resolvePath(ValueRef root, std::string_view path);

} // namespace ox::reflect

#define OX_REFLECT_TYPE(T, Name) ::ox::reflect::registerType<T>(Name)
#define OX_REFLECT_ENUM(E, Name) ::ox::reflect::registerType<E>(Name)

// ValueRef::getAs/setAs need the converters from serial/convert.hpp.
#include <oxwald/core/serial/convert.hpp>

namespace ox::reflect {
template <class T>
std::optional<T> ValueRef::getAs() const {
    if (!valid()) return std::nullopt;
    if (auto* p = as<T>()) return *p;
    T out{};
    if (!serial::fromValue(get(), &out, typeOf<T>())) return std::nullopt;
    return out;
}

template <class T>
bool ValueRef::setAs(const T& value) const {
    if (!valid()) return false;
    if (auto* p = as<T>()) {
        *p = value;
        return true;
    }
    return set(serial::toValue(&value, typeOf<T>()));
}
} // namespace ox::reflect
