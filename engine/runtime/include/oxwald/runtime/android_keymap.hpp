#pragma once

// Android key codes / motion axes -> engine input (pure tables, no NDK headers: host-testable). The numeric values
// are the stable AKEYCODE_* / AMOTION_EVENT_AXIS_* constants of <android/keycodes.h> / <android/input.h>.
#include <oxwald/runtime/input.hpp>

#include <array>
#include <optional>
#include <span>

namespace ox::android_input {

// AMOTION_EVENT_AXIS_* used by gamepads.
inline constexpr i32 kAxisX = 0, kAxisY = 1, kAxisZ = 11, kAxisRX = 12, kAxisRY = 13, kAxisRZ = 14, kAxisHatX = 15,
                     kAxisHatY = 16, kAxisLTrigger = 17, kAxisRTrigger = 18, kAxisGas = 22, kAxisBrake = 23;
inline constexpr i32 kAxisCount = 48;
inline constexpr i32 kKeycodeBack = 4;

// Keyboard keys (hardware keyboards, emulator host keyboard). BACK maps to Escape (menus). nullopt = unmapped.
[[nodiscard]] std::optional<Key> keyFromKeycode(i32 keycode);
// Gamepad buttons (AKEYCODE_BUTTON_*, and DPAD_* when the event comes from a gamepad).
[[nodiscard]] std::optional<GamepadButton> gamepadButtonFromKeycode(i32 keycode);

struct GamepadSample {
    std::array<f32, kGamepadAxisCount> axes{}; // engine layout: sticks +Y up, triggers [0,1]
    // D-pad from the hat axes (controllers that report it as axes instead of DPAD key events).
    bool dpadUp = false, dpadRight = false, dpadDown = false, dpadLeft = false;
};

// `values[axis]` = AMotionEvent_getAxisValue(event, axis, pointer) for axis in [0, kAxisCount). Right stick:
// Z/RZ (HID standard, Xbox/DualSense on Android) or RX/RY when Z/RZ are idle; triggers: L/RTRIGGER or BRAKE/GAS.
[[nodiscard]] GamepadSample gamepadFromAxes(std::span<const f32> values);

} // namespace ox::android_input
