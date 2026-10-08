#include "integration/input_bridge.hpp"

#include <QKeyEvent>

#if OX_EDITOR_HAS_RUNTIME
#include <oxwald/runtime/engine.hpp>
#include <oxwald/runtime/input.hpp>
#endif

#include <optional>

namespace ox::editor {

#if OX_EDITOR_HAS_RUNTIME
namespace {

std::optional<Key> mapKey(int k, Qt::KeyboardModifiers mods) {
    const bool keypad = mods & Qt::KeypadModifier;
    if (k >= Qt::Key_A && k <= Qt::Key_Z) return Key(int(Key::A) + (k - Qt::Key_A));
    if (k >= Qt::Key_0 && k <= Qt::Key_9) return Key(int(keypad ? Key::Kp0 : Key::Num0) + (k - Qt::Key_0));
    if (k >= Qt::Key_F1 && k <= Qt::Key_F12) return Key(int(Key::F1) + (k - Qt::Key_F1));
    switch (k) {
    case Qt::Key_Space: return Key::Space;
    case Qt::Key_Apostrophe: return Key::Apostrophe;
    case Qt::Key_Comma: return Key::Comma;
    case Qt::Key_Minus: return keypad ? Key::KpSubtract : Key::Minus;
    case Qt::Key_Period: return keypad ? Key::KpDecimal : Key::Period;
    case Qt::Key_Slash: return keypad ? Key::KpDivide : Key::Slash;
    case Qt::Key_Semicolon: return Key::Semicolon;
    case Qt::Key_Equal: return keypad ? Key::KpEqual : Key::Equal;
    case Qt::Key_BracketLeft: return Key::LeftBracket;
    case Qt::Key_Backslash: return Key::Backslash;
    case Qt::Key_BracketRight: return Key::RightBracket;
    case Qt::Key_QuoteLeft: return Key::GraveAccent;
    case Qt::Key_Escape: return Key::Escape;
    case Qt::Key_Return: return Key::Enter;
    case Qt::Key_Enter: return keypad ? Key::KpEnter : Key::Enter;
    case Qt::Key_Tab: return Key::Tab;
    case Qt::Key_Backspace: return Key::Backspace;
    case Qt::Key_Insert: return Key::Insert;
    case Qt::Key_Delete: return Key::Delete;
    case Qt::Key_Right: return Key::Right;
    case Qt::Key_Left: return Key::Left;
    case Qt::Key_Down: return Key::Down;
    case Qt::Key_Up: return Key::Up;
    case Qt::Key_PageUp: return Key::PageUp;
    case Qt::Key_PageDown: return Key::PageDown;
    case Qt::Key_Home: return Key::Home;
    case Qt::Key_End: return Key::End;
    case Qt::Key_CapsLock: return Key::CapsLock;
    case Qt::Key_ScrollLock: return Key::ScrollLock;
    case Qt::Key_NumLock: return Key::NumLock;
    case Qt::Key_Print: return Key::PrintScreen;
    case Qt::Key_Pause: return Key::Pause;
    case Qt::Key_Asterisk: return Key::KpMultiply;
    case Qt::Key_Plus: return Key::KpAdd;
    case Qt::Key_Shift: return Key::LeftShift;
    // macOS: Qt::Key_Control is Command, Qt::Key_Meta is Control.
    case Qt::Key_Control: return Key::LeftControl;
    case Qt::Key_Meta: return Key::LeftSuper;
    case Qt::Key_Alt: return Key::LeftAlt;
    case Qt::Key_Menu: return Key::Menu;
    default: return std::nullopt;
    }
}

} // namespace

bool forwardKey(Engine* engine, const QKeyEvent* event, bool down) {
    if (!engine || !event) return false;
    auto key = mapKey(event->key(), event->modifiers());
    if (!key) return false;
    engine->input().inject(InputEvent::key(*key, down, event->isAutoRepeat()));
    if (down && !event->text().isEmpty()) {
        for (const QChar c : event->text()) {
            if (c.isPrint()) engine->input().inject(InputEvent::text(c.unicode()));
        }
    }
    return true;
}

void forwardMouseButton(Engine* engine, Qt::MouseButton button, bool down) {
    if (!engine) return;
    MouseButton b;
    switch (button) {
    case Qt::LeftButton: b = MouseButton::Left; break;
    case Qt::RightButton: b = MouseButton::Right; break;
    case Qt::MiddleButton: b = MouseButton::Middle; break;
    case Qt::BackButton: b = MouseButton::Button4; break;
    case Qt::ForwardButton: b = MouseButton::Button5; break;
    default: return;
    }
    engine->input().inject(InputEvent::mouseButton(b, down));
}

void forwardMouseMove(Engine* engine, QPointF position, QPointF delta) {
    if (!engine) return;
    engine->input().inject(InputEvent::mouseMove({float(position.x()), float(position.y())}, {float(delta.x()), float(delta.y())}));
}

void forwardWheel(Engine* engine, QPointF degrees) {
    if (!engine) return;
    engine->input().inject(InputEvent::mouseWheel({float(degrees.x() / 15.0), float(degrees.y() / 15.0)}));
}

void forwardFocusLost(Engine* engine) {
    if (engine) engine->input().inject(InputEvent::focusLost());
}

int requestedCursorMode(Engine* engine) { return engine ? int(engine->input().cursorMode()) : -1; }

QString keySourceForQtKey(int qtKey, Qt::KeyboardModifiers modifiers) {
    auto key = mapKey(qtKey, modifiers);
    if (!key) return {};
    const std::string_view n = keyName(*key);
    return QStringLiteral("Key.") + QString::fromLatin1(n.data(), qsizetype(n.size()));
}

#else

QString keySourceForQtKey(int, Qt::KeyboardModifiers) { return {}; }

bool forwardKey(Engine*, const QKeyEvent*, bool) { return false; }
void forwardMouseButton(Engine*, Qt::MouseButton, bool) {}
void forwardMouseMove(Engine*, QPointF, QPointF) {}
void forwardWheel(Engine*, QPointF) {}
void forwardFocusLost(Engine*) {}
int requestedCursorMode(Engine*) { return -1; }

#endif

} // namespace ox::editor
