#include <oxwald/runtime/input.hpp>
#include <oxwald/runtime/script_bindings.hpp>

#if OX_HAS_SCRIPT

#include <oxwald/script/script_vm.hpp>

namespace ox {

void bindInputLuaApi(script::ScriptVM& vm, InputSystem& input) {
    InputSystem* in = &input;
    vm.bindApi("input", [in](sol::state_view lua, sol::table& api) {
        api["keyDown"] = [in](const std::string& k) { auto key = keyFromName(k); return key && in->keyDown(*key); };
        api["keyPressed"] = [in](const std::string& k) { auto key = keyFromName(k); return key && in->keyPressed(*key); };
        api["keyReleased"] = [in](const std::string& k) {
            auto key = keyFromName(k);
            return key && in->keyReleased(*key);
        };
        api["mouseDown"] = [in](const std::string& b) {
            auto src = parseInputSource("Mouse." + b);
            return src && src->device == InputSource::Device::MouseButton && in->mouseDown(MouseButton(src->code));
        };
        api["mousePosition"] = [in]() { return in->mousePosition(); };
        api["mouseDelta"] = [in]() { return in->mouseDelta(); };
        api["mouseWheel"] = [in]() { return in->mouseWheel().y; };
        api["triggered"] = [in](const std::string& a) { return in->triggered(a); };
        api["started"] = [in](const std::string& a) { return in->started(a); };
        api["completed"] = [in](const std::string& a) { return in->completed(a); };
        api["action"] = [in, lua](const std::string& a) -> sol::object {
            const ActionState* s = in->action(a);
            if (!s) return sol::make_object(lua, sol::lua_nil);
            switch (s->type) {
            case InputValueType::Bool: return sol::make_object(lua, s->boolValue());
            case InputValueType::Axis1D: return sol::make_object(lua, s->value.x);
            case InputValueType::Axis2D: return sol::make_object(lua, s->axis2D());
            case InputValueType::Axis3D: return sol::make_object(lua, s->value);
            }
            return sol::make_object(lua, sol::lua_nil);
        };
        api["gamepadAxis"] = [in](int pad, const std::string& axis) -> f32 {
            auto src = parseInputSource("Gamepad." + axis);
            if (!src || src->device != InputSource::Device::GamepadAxis || pad < 0) return 0.0f;
            return in->gamepadAxis(u32(pad), GamepadAxis(src->code));
        };
        api["setCursorMode"] = [in](const std::string& mode) {
            in->setCursorMode(mode == "Locked" ? CursorMode::Locked : mode == "Hidden" ? CursorMode::Hidden : CursorMode::Normal);
        };
    });
}

} // namespace ox

#endif
