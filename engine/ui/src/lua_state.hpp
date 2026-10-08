#pragma once

// Lua binding state of UiSystem (complete type for ui_system.cpp and ui_lua.cpp).

#include <oxwald/ui/ui_system.hpp>

#if OX_UI_HAS_SCRIPT
#include <oxwald/script/script_vm.hpp>

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Variant.h>

#include <map>
#include <string>
#include <vector>

namespace ox::ui {

struct LuaModel {
    std::string name;
    std::map<std::string, Rml::Variant, std::less<>> values;
    std::map<std::string, sol::protected_function, std::less<>> events;
    Rml::DataModelConstructor constructor;
    Rml::DataModelHandle handle;
};


struct UiSystem::LuaState {
    script::ScriptVM* vm = nullptr;
    UiSystem* ui = nullptr;
    int openWindows = 0;
    std::map<std::string, std::unique_ptr<LuaModel>, std::less<>> models;
    std::vector<u64> listeners;

    bool imguiReady() const { return ui->imgui() && ui->imgui()->inFrame(); }
};

} // namespace ox::ui

#else

namespace ox::ui {
struct UiSystem::LuaState {};
} // namespace ox::ui

#endif
