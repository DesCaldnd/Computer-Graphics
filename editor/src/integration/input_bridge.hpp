#pragma once

#include <QPointF>
#include <QString>
#include <Qt>

class QKeyEvent;

namespace ox {
class Engine;
}

namespace ox::editor {

// Forwards viewport input to the runtime InputSystem while playing (no-ops without the runtime module).
// Qt keys map to ox::Key by name (letters, digits, F-keys, arrows, modifiers, keypad when Qt::KeypadModifier).
bool forwardKey(Engine* engine, const QKeyEvent* event, bool down);
void forwardMouseButton(Engine* engine, Qt::MouseButton button, bool down);
void forwardMouseMove(Engine* engine, QPointF position, QPointF delta);
void forwardWheel(Engine* engine, QPointF degrees);
void forwardFocusLost(Engine* engine);
// "Key.<Name>" binding source for a Qt key (Project Settings key capture); empty when unmapped.
[[nodiscard]] QString keySourceForQtKey(int qtKey, Qt::KeyboardModifiers modifiers);
// Cursor mode requested by gameplay (InputSystem::setCursorMode): 0 Normal, 1 Hidden, 2 Locked; -1 without runtime.
[[nodiscard]] int requestedCursorMode(Engine* engine);

} // namespace ox::editor
