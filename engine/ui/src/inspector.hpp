#pragma once

// Reflection-driven ImGui property editing (entity inspector, Lua-free). Returns true when a value changed.

#include <oxwald/core/reflect.hpp>

namespace ox::ui {

bool inspectValue(const char* label, const reflect::TypeInfo& type, void* data, const reflect::Attributes& attributes,
                  int depth = 0);
bool inspectStruct(const reflect::TypeInfo& type, void* data, int depth = 0);

} // namespace ox::ui
