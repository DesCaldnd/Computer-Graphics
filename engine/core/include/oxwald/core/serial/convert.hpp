#pragma once

#include <oxwald/core/serial/value.hpp>

#include <functional>

namespace ox::reflect {
struct TypeInfo;
struct FieldInfo;
template <class T>
const TypeInfo& typeOf();
} // namespace ox::reflect

// Conversion between reflected C++ objects and the archive value tree.
namespace ox::serial {

struct ConvertOptions {
    // Return false to skip a field (applied recursively). Fields with attr::NoSerialize are always skipped.
    std::function<bool(const reflect::FieldInfo&)> fieldFilter;
};

// Wire descriptor of a reflected type: structs -> "object", vector<T> -> "array<T>", enums -> "enum" (or "i64"
// for enums registered without names), custom leaves -> their tag.
[[nodiscard]] DescPtr descOf(const reflect::TypeInfo& type);

[[nodiscard]] Value toValue(const void* object, const reflect::TypeInfo& type, const ConvertOptions& options = {});

// Tolerant conversion: numeric types convert, vectors accept arrays, enums accept names/numbers, unknown fields
// are ignored, missing fields keep the object's current values, renamed fields resolve via attr::FormerName.
// Returns false only when the value is incompatible with the type at the top level.
bool fromValue(const Value& value, void* object, const reflect::TypeInfo& type, const ConvertOptions& options = {});

template <class T>
[[nodiscard]] Value toValue(const T& object, const ConvertOptions& options = {}) {
    return toValue(&object, reflect::typeOf<T>(), options);
}

template <class T>
bool fromValue(const Value& value, T& object, const ConvertOptions& options = {}) {
    return fromValue(value, &object, reflect::typeOf<T>(), options);
}

} // namespace ox::serial
