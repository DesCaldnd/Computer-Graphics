#pragma once

// Touch screen -> Enhanced Input. Platform layers (Android GameActivity, tests) feed raw pointer events; the mapper
// turns them into ordinary InputEvents so existing action bindings work unchanged on phones:
//
//   * virtual stick   a finger that lands in `moveZone` (default: left half) becomes a floating stick centred where
//                     it touched; its deflection drives a virtual gamepad's left stick (Gamepad.LeftStick /
//                     Gamepad.LeftX / LeftY bindings, +Y = up).
//   * look            a finger elsewhere moves the mouse (Mouse.XY / Mouse.X / Mouse.Y deltas scaled by
//                     lookSensitivity) and the cursor position (UI hover).
//   * tap             a short look-finger touch that barely moved is a Mouse.Left click at that position (RmlUi /
//                     ImGui buttons, Mouse.Left bindings).
//   * buttons         on-screen rectangles bound to sources ("Gamepad.A", "Key.Space", "Mouse.Right", ...): pressed
//                     while a finger is on them.
//   * pinch           two look fingers moving apart/together scroll the mouse wheel (Mouse.Wheel, zoom).
//
// Coordinates are window pixels; zones are normalised [0,1] rectangles {x0, y0, x1, y1} (y down). The virtual pad
// uses its own slot (default the last one) so a real controller keeps slot 0. Not thread-safe: one mapper per
// platform, called from the thread that pumps OS events; the produced InputEvents go to InputSystem::inject().
#include <oxwald/runtime/input.hpp>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <optional>
#include <string>
#include <vector>

namespace ox {

enum class TouchPhase : u8 { Down, Move, Up, Cancel };

struct TouchEvent {
    TouchPhase phase = TouchPhase::Down;
    i32 pointerId = 0;      // stable while the finger is down
    glm::vec2 position{0};  // window pixels
    f64 time = 0.0;         // seconds (any monotonic clock)
};

struct TouchButton {
    std::string name;
    glm::vec4 rect{0.0f};             // normalised {x0, y0, x1, y1}
    std::vector<std::string> sources; // input source names (parseInputSource)
};

struct TouchLayout {
    glm::vec4 moveZone{0.0f, 0.0f, 0.5f, 1.0f};
    f32 stickRadius = 0.12f;      // full deflection distance, fraction of the viewport height
    f32 stickDeadZone = 0.08f;    // normalised; the action bindings may add their own dead zones
    f32 lookSensitivity = 1.0f;   // mouse delta per pixel of finger motion
    f32 tapMaxSeconds = 0.3f;
    f32 tapMaxDistance = 0.02f;   // fraction of the viewport height
    f32 pinchWheelPerPixel = 0.02f;
    u32 gamepadSlot = kMaxGamepads - 1;
    std::vector<TouchButton> buttons; // checked before the stick / look zones

    // Stick on the left half, look on the right, Jump (Gamepad.A + Key.Space) and Interact (Gamepad.X + Key.E)
    // buttons in the bottom-right corner.
    [[nodiscard]] static TouchLayout defaults();
};

class TouchInputMapper {
public:
    explicit TouchInputMapper(TouchLayout layout = TouchLayout::defaults());

    void setLayout(TouchLayout layout);
    [[nodiscard]] const TouchLayout& layout() const { return m_layout; }
    // Window size in pixels (zones and distances are relative to it).
    void setViewport(glm::vec2 size) { m_viewport = size; }
    [[nodiscard]] glm::vec2 viewport() const { return m_viewport; }

    // Appends the InputEvents caused by one touch event.
    void process(const TouchEvent& event, std::vector<InputEvent>& out);
    // Releases everything that is held (focus lost, app paused, touch stream cancelled).
    void reset(std::vector<InputEvent>& out);

    [[nodiscard]] glm::vec2 stickValue() const { return m_stickValue; } // +Y up, |v| <= 1
    [[nodiscard]] usize activePointers() const { return m_pointers.size(); }
    [[nodiscard]] bool stickActive() const;

private:
    enum class Role : u8 { Stick, Look, Button };
    struct Pointer {
        i32 id = 0;
        Role role = Role::Look;
        usize button = 0;
        glm::vec2 origin{0};
        glm::vec2 last{0};
        f64 downTime = 0.0;
        f32 travelled = 0.0f; // max distance from origin
    };

    [[nodiscard]] glm::vec2 normalized(glm::vec2 px) const;
    [[nodiscard]] static bool inside(glm::vec4 rect, glm::vec2 n);
    Pointer* find(i32 id);
    void emitStick(glm::vec2 value, std::vector<InputEvent>& out);
    void emitButton(usize index, bool pressed, std::vector<InputEvent>& out);
    void ensurePad(std::vector<InputEvent>& out);
    [[nodiscard]] f32 pinchDistance() const;

    TouchLayout m_layout;
    std::vector<std::vector<InputSource>> m_buttonSources;
    glm::vec2 m_viewport{1.0f, 1.0f};
    std::vector<Pointer> m_pointers;
    glm::vec2 m_stickValue{0};
    bool m_padAnnounced = false;
    std::optional<f32> m_pinch; // distance between the first two look fingers
};

} // namespace ox
