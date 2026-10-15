#include <oxwald/runtime/android_keymap.hpp>

#include <algorithm>
#include <cmath>

namespace ox::android_input {

std::optional<Key> keyFromKeycode(i32 k) {
    if (k >= 7 && k <= 16) return Key(u16(Key::Num0) + u16(k - 7));   // AKEYCODE_0..9
    if (k >= 29 && k <= 54) return Key(u16(Key::A) + u16(k - 29));    // AKEYCODE_A..Z
    if (k >= 131 && k <= 142) return Key(u16(Key::F1) + u16(k - 131)); // AKEYCODE_F1..F12
    if (k >= 144 && k <= 153) return Key(u16(Key::Kp0) + u16(k - 144)); // AKEYCODE_NUMPAD_0..9
    switch (k) {
    case kKeycodeBack: return Key::Escape;
    case 19: return Key::Up;    // DPAD_UP (arrow keys on keyboards)
    case 20: return Key::Down;
    case 21: return Key::Left;
    case 22: return Key::Right;
    case 55: return Key::Comma;
    case 56: return Key::Period;
    case 57: return Key::LeftAlt;
    case 58: return Key::RightAlt;
    case 59: return Key::LeftShift;
    case 60: return Key::RightShift;
    case 61: return Key::Tab;
    case 62: return Key::Space;
    case 66: return Key::Enter;
    case 67: return Key::Backspace; // AKEYCODE_DEL
    case 68: return Key::GraveAccent;
    case 69: return Key::Minus;
    case 70: return Key::Equal;
    case 71: return Key::LeftBracket;
    case 72: return Key::RightBracket;
    case 73: return Key::Backslash;
    case 74: return Key::Semicolon;
    case 75: return Key::Apostrophe;
    case 76: return Key::Slash;
    case 82: return Key::Menu;
    case 92: return Key::PageUp;
    case 93: return Key::PageDown;
    case 111: return Key::Escape;
    case 112: return Key::Delete; // FORWARD_DEL
    case 113: return Key::LeftControl;
    case 114: return Key::RightControl;
    case 115: return Key::CapsLock;
    case 116: return Key::ScrollLock;
    case 117: return Key::LeftSuper;
    case 118: return Key::RightSuper;
    case 120: return Key::PrintScreen; // SYSRQ
    case 121: return Key::Pause;       // BREAK
    case 122: return Key::Home;        // MOVE_HOME
    case 123: return Key::End;         // MOVE_END
    case 124: return Key::Insert;
    case 143: return Key::NumLock;
    case 154: return Key::KpDivide;
    case 155: return Key::KpMultiply;
    case 156: return Key::KpSubtract;
    case 157: return Key::KpAdd;
    case 158: return Key::KpDecimal;
    case 160: return Key::KpEnter;
    case 161: return Key::KpEqual;
    default: return std::nullopt;
    }
}

std::optional<GamepadButton> gamepadButtonFromKeycode(i32 k) {
    switch (k) {
    case 96: return GamepadButton::A;  // BUTTON_A
    case 97: return GamepadButton::B;
    case 99: return GamepadButton::X;
    case 100: return GamepadButton::Y;
    case 102: return GamepadButton::LeftBumper;  // L1
    case 103: return GamepadButton::RightBumper; // R1
    case 106: return GamepadButton::LeftThumb;
    case 107: return GamepadButton::RightThumb;
    case 108: return GamepadButton::Start;
    case 109: return GamepadButton::Back; // SELECT
    case 110: return GamepadButton::Guide; // MODE
    case 19: return GamepadButton::DpadUp;
    case 20: return GamepadButton::DpadDown;
    case 21: return GamepadButton::DpadLeft;
    case 22: return GamepadButton::DpadRight;
    case 23: return GamepadButton::A; // DPAD_CENTER (TV remotes)
    case kKeycodeBack: return GamepadButton::B; // controllers send BACK for B when not mapped
    default: return std::nullopt;
    }
}

GamepadSample gamepadFromAxes(std::span<const f32> v) {
    auto at = [&](i32 axis) { return axis < i32(v.size()) ? v[usize(axis)] : 0.0f; };
    GamepadSample s;
    s.axes[usize(GamepadAxis::LeftX)] = at(kAxisX);
    s.axes[usize(GamepadAxis::LeftY)] = -at(kAxisY);
    const bool zActive = std::abs(at(kAxisZ)) > 1e-4f || std::abs(at(kAxisRZ)) > 1e-4f;
    s.axes[usize(GamepadAxis::RightX)] = zActive ? at(kAxisZ) : at(kAxisRX);
    s.axes[usize(GamepadAxis::RightY)] = -(zActive ? at(kAxisRZ) : at(kAxisRY));
    s.axes[usize(GamepadAxis::LeftTrigger)] = std::clamp(std::max(at(kAxisLTrigger), at(kAxisBrake)), 0.0f, 1.0f);
    s.axes[usize(GamepadAxis::RightTrigger)] = std::clamp(std::max(at(kAxisRTrigger), at(kAxisGas)), 0.0f, 1.0f);
    const f32 hx = at(kAxisHatX), hy = at(kAxisHatY);
    s.dpadLeft = hx < -0.5f;
    s.dpadRight = hx > 0.5f;
    s.dpadUp = hy < -0.5f;
    s.dpadDown = hy > 0.5f;
    return s;
}

} // namespace ox::android_input
