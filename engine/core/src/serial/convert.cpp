#include <oxwald/core/log.hpp>
#include <oxwald/core/reflect.hpp>
#include <oxwald/core/serial/convert.hpp>

#include <cstdlib>
#include <cstring>

namespace ox::serial {

using reflect::Kind;
using reflect::TypeInfo;

DescPtr descOf(const TypeInfo& type) {
    switch (type.kind) {
    case Kind::Struct: return TypeDesc::of(Tag::Object);
    case Kind::Enum: return TypeDesc::of(type.enumEntries.empty() ? Tag::I64 : Tag::Enum);
    case Kind::Array: return TypeDesc::arrayOf(descOf(*type.element));
    case Kind::Optional: return TypeDesc::optionalOf(descOf(*type.element));
    case Kind::Map: return TypeDesc::mapOf(descOf(*type.element));
    default: return TypeDesc::of(type.tag);
    }
}

namespace {

template <class T>
T load(const void* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

template <class T>
void store(void* p, T v) {
    std::memcpy(p, &v, sizeof(T));
}

Value leafToValue(const void* p, const TypeInfo& type) {
    switch (type.tag) {
    case Tag::Bool: return Value::makeBool(*static_cast<const bool*>(p));
    case Tag::I8: return Value::makeInt(load<i8>(p), Tag::I8);
    case Tag::I16: return Value::makeInt(load<i16>(p), Tag::I16);
    case Tag::I32: return Value::makeInt(load<i32>(p), Tag::I32);
    case Tag::I64: return Value::makeInt(load<i64>(p), Tag::I64);
    case Tag::U8: return Value::makeUInt(load<u8>(p), Tag::U8);
    case Tag::U16: return Value::makeUInt(load<u16>(p), Tag::U16);
    case Tag::U32: return Value::makeUInt(load<u32>(p), Tag::U32);
    case Tag::U64: return Value::makeUInt(load<u64>(p), Tag::U64);
    case Tag::F32: return Value::makeF32(load<f32>(p));
    case Tag::F64: return Value::makeF64(load<f64>(p));
    case Tag::String: return Value::makeString(*static_cast<const std::string*>(p));
    case Tag::Vec2: return Value::makeVec2(load<glm::vec2>(p));
    case Tag::Vec3: return Value::makeVec3(load<glm::vec3>(p));
    case Tag::Vec4: return Value::makeVec4(load<glm::vec4>(p));
    case Tag::IVec2: return Value::makeIVec2(load<glm::ivec2>(p));
    case Tag::IVec3: return Value::makeIVec3(load<glm::ivec3>(p));
    case Tag::IVec4: return Value::makeIVec4(load<glm::ivec4>(p));
    case Tag::Quat: return Value::makeQuat(*static_cast<const glm::quat*>(p));
    case Tag::Mat4: return Value::makeMat4(*static_cast<const glm::mat4*>(p));
    case Tag::Uuid: return Value::makeUuid(*static_cast<const Uuid*>(p));
    default: return {};
    }
}

bool leafFromValue(const Value& v, void* p, const TypeInfo& type) {
    switch (type.tag) {
    case Tag::Bool:
        if (v.tag() != Tag::Bool && !v.isNumber()) return false;
        *static_cast<bool*>(p) = v.getBool();
        return true;
    case Tag::I8:
    case Tag::I16:
    case Tag::I32:
    case Tag::I64:
    case Tag::U8:
    case Tag::U16:
    case Tag::U32:
    case Tag::U64: {
        if (!v.isNumber() && v.tag() != Tag::Bool) return false;
        // Little-endian: writing the low bytes of the 64-bit value yields the narrow value.
        const u64 bits = v.tag() == Tag::U64 ? v.getUInt() : static_cast<u64>(v.getInt());
        std::memcpy(p, &bits, type.size);
        return true;
    }
    case Tag::F32:
        if (!v.isNumber()) return false;
        store<f32>(p, static_cast<f32>(v.getDouble()));
        return true;
    case Tag::F64:
        if (!v.isNumber()) return false;
        store<f64>(p, v.getDouble());
        return true;
    case Tag::String:
        if (v.tag() != Tag::String && v.tag() != Tag::Enum) return false;
        *static_cast<std::string*>(p) = v.getString();
        return true;
    case Tag::Vec2:
    case Tag::Vec3:
    case Tag::Vec4: {
        const Tag t = v.tag();
        if (!(t >= Tag::Vec2 && t <= Tag::Quat) && t != Tag::Array) return false;
        const glm::vec4 x = v.getVec();
        std::memcpy(p, &x, type.size);
        return true;
    }
    case Tag::IVec2:
    case Tag::IVec3:
    case Tag::IVec4: {
        const Tag t = v.tag();
        if (!(t >= Tag::Vec2 && t <= Tag::IVec4) && t != Tag::Array) return false;
        const glm::ivec4 x = v.getIVec();
        std::memcpy(p, &x, type.size);
        return true;
    }
    case Tag::Quat:
        if (v.tag() != Tag::Quat && v.tag() != Tag::Vec4 && v.tag() != Tag::Array) return false;
        *static_cast<glm::quat*>(p) = v.getQuat();
        return true;
    case Tag::Mat4:
        if (v.tag() != Tag::Mat4 && v.tag() != Tag::Array) return false;
        *static_cast<glm::mat4*>(p) = v.getMat4();
        return true;
    case Tag::Uuid:
        if (v.tag() != Tag::Uuid && v.tag() != Tag::EntityRef && v.tag() != Tag::String) return false;
        *static_cast<Uuid*>(p) = v.getUuid();
        return true;
    default: return false;
    }
}

bool includeField(const reflect::FieldInfo& f, const ConvertOptions& options) {
    if (f.attributes.noSerialize) return false;
    return !options.fieldFilter || options.fieldFilter(f);
}

} // namespace

Value toValue(const void* object, const TypeInfo& type, const ConvertOptions& options) {
    switch (type.kind) {
    case Kind::Struct: {
        Value obj = Value::makeObject(type.name);
        obj.reserve(type.fields.size());
        for (const auto& f : type.fields) {
            if (!includeField(f, options)) continue;
            obj.fields().emplace_back(f.name, toValue(f.get(object), *f.type, options));
        }
        return obj;
    }
    case Kind::Enum: {
        const i64 raw = type.getEnum(object);
        if (type.enumEntries.empty()) return Value::makeInt(raw, Tag::I64);
        if (const auto* e = type.findEnumByValue(raw)) return Value::makeEnumHash(e->nameHash, e->name);
        // Value outside the registered set (e.g. flag combinations): keep it numerically.
        return Value::makeEnum(std::to_string(raw));
    }
    case Kind::Array: {
        Value arr = Value::makeArray(descOf(*type.element));
        auto* mut = const_cast<void*>(object);
        const usize n = type.containerSize(object);
        if (type.packedLayout && fixedSize(type.element->tag) == type.element->size && n > 0) {
            const auto* data = static_cast<const std::byte*>(type.arrayData(mut));
            return Value::makeRawArray(descOf(*type.element), std::vector<std::byte>(data, data + n * type.element->size));
        }
        arr.reserve(n);
        for (usize i = 0; i < n; ++i) arr.push(toValue(type.arrayAt(mut, i), *type.element, options));
        return arr;
    }
    case Kind::Optional: {
        auto* mut = const_cast<void*>(object);
        if (!type.optionalHas(object)) return Value::makeOptional(descOf(*type.element));
        return Value::makeOptional(descOf(*type.element), toValue(type.optionalGet(mut), *type.element, options));
    }
    case Kind::Map: {
        Value map = Value::makeMap(descOf(*type.element));
        type.mapForEach(object, [&](std::string_view key, const void* v) {
            map.fields().emplace_back(std::string(key), toValue(v, *type.element, options));
        });
        return map;
    }
    case Kind::Custom: return type.customToValue(object);
    default: return leafToValue(object, type);
    }
}

bool fromValue(const Value& value, void* object, const TypeInfo& type, const ConvertOptions& options) {
    switch (type.kind) {
    case Kind::Struct: {
        if (value.tag() != Tag::Object && value.tag() != Tag::Map) return false;
        for (const auto& [name, fieldValue] : value.fields()) {
            const reflect::FieldInfo* f = type.findFieldOrFormer(name);
            if (!f || !includeField(*f, options)) continue; // unknown/removed field
            if (!fromValue(fieldValue, f->get(object), *f->type, options)) {
                OX_LOG_WARN("serial", "{}.{}: cannot convert {} value, keeping default", type.name, name,
                            tagName(fieldValue.tag()));
            }
        }
        return true;
    }
    case Kind::Enum: {
        if (value.tag() == Tag::Enum || value.tag() == Tag::String) {
            const reflect::EnumEntry* e = value.tag() == Tag::String ? type.findEnum(value.getString())
                                                                     : type.findEnumByHash(value.enumHash());
            if (!e && !value.getString().empty()) e = type.findEnum(value.getString());
            if (e) {
                type.setEnum(object, e->value);
                return true;
            }
            // Numeric fallback for out-of-range values written as their decimal string.
            const std::string& s = value.getString();
            char* end = nullptr;
            const long long n = std::strtoll(s.c_str(), &end, 10);
            if (!s.empty() && end && *end == '\0') {
                type.setEnum(object, n);
                return true;
            }
            return false;
        }
        if (value.isNumber()) {
            type.setEnum(object, value.getInt());
            return true;
        }
        return false;
    }
    case Kind::Array: {
        if (value.tag() != Tag::Array) return false;
        const usize n = value.size();
        type.arrayResize(object, n);
        if (n == 0) return true;
        if (value.isPacked() && type.packedLayout && value.elemDesc()->tag == type.element->tag &&
            fixedSize(type.element->tag) == type.element->size) {
            std::memcpy(type.arrayData(object), value.packedBytes().data(), value.packedBytes().size());
            return true;
        }
        for (usize i = 0; i < n; ++i) {
            if (value.isPacked()) {
                fromValue(value.at(i), type.arrayAt(object, i), *type.element, options);
            } else {
                fromValue(value.items()[i], type.arrayAt(object, i), *type.element, options);
            }
        }
        return true;
    }
    case Kind::Optional: {
        if (value.isNull() || (value.isOptional() && !value.hasValue())) {
            type.containerClear(object);
            return true;
        }
        const Value& inner = value.isOptional() ? value.optionalValue() : value;
        void* target = type.optionalEmplace(object);
        return fromValue(inner, target, *type.element, options);
    }
    case Kind::Map: {
        if (value.tag() != Tag::Map && value.tag() != Tag::Object) return false;
        type.containerClear(object);
        for (const auto& [key, v] : value.fields()) {
            fromValue(v, type.mapInsert(object, key), *type.element, options);
        }
        return true;
    }
    case Kind::Custom: return type.customFromValue(object, value);
    default: return leafFromValue(value, object, type);
    }
}

} // namespace ox::serial
