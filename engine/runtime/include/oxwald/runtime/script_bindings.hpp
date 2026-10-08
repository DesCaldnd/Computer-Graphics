#pragma once

// Lua bindings for runtime services (only when the script module is linked: OX_HAS_SCRIPT).
#if OX_HAS_SCRIPT

namespace ox {
class InputSystem;
namespace script {
class ScriptVM;
}

// Global read-only table `input` in every sandbox:
//   input.keyDown("W"), input.keyPressed("Space"), input.keyReleased("E"), input.mouseDown("Left"),
//   input.mousePosition() -> vec2, input.mouseDelta() -> vec2, input.mouseWheel() -> number,
//   input.action("Move") -> bool | number | vec2 | vec3 (by action type), input.triggered("Jump"),
//   input.started("Fire"), input.completed("Fire"), input.gamepadAxis(pad, "LeftX"),
//   input.setCursorMode("Normal" | "Hidden" | "Locked")
void bindInputLuaApi(script::ScriptVM& vm, InputSystem& input);

} // namespace ox

#endif
