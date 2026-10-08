#pragma once

#include <oxwald/core/events.hpp>
#include <oxwald/core/types.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <bitset>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Device-agnostic input with UE "Enhanced Input"-style action mapping.
//
// Platform layers (GlfwPlatform, the Qt editor viewport, tests) inject raw events from any thread with
// InputSystem::inject(); the game thread calls update(dt) once per frame, which applies the queued events and
// evaluates actions. Bindings refer to sources by name ("Key.W", "Mouse.Left", "Mouse.XY", "Gamepad.LeftStick"),
// pass the raw value through modifiers (dead zone, negate, swizzle, scale) and triggers (down, pressed, released,
// hold, tap, chord) and feed actions (bool / 1D / 2D / 3D). Contexts are prioritised: sources bound in a higher
// priority context are consumed and invisible to lower ones.
namespace ox {

#define OX_INPUT_KEYS(X)                                                                                              \
    X(Unknown) X(Space) X(Apostrophe) X(Comma) X(Minus) X(Period) X(Slash) X(Num0) X(Num1) X(Num2) X(Num3) X(Num4)   \
    X(Num5) X(Num6) X(Num7) X(Num8) X(Num9) X(Semicolon) X(Equal) X(A) X(B) X(C) X(D) X(E) X(F) X(G) X(H) X(I) X(J)  \
    X(K) X(L) X(M) X(N) X(O) X(P) X(Q) X(R) X(S) X(T) X(U) X(V) X(W) X(X) X(Y) X(Z) X(LeftBracket) X(Backslash)      \
    X(RightBracket) X(GraveAccent) X(Escape) X(Enter) X(Tab) X(Backspace) X(Insert) X(Delete) X(Right) X(Left)       \
    X(Down) X(Up) X(PageUp) X(PageDown) X(Home) X(End) X(CapsLock) X(ScrollLock) X(NumLock) X(PrintScreen) X(Pause)  \
    X(F1) X(F2) X(F3) X(F4) X(F5) X(F6) X(F7) X(F8) X(F9) X(F10) X(F11) X(F12) X(Kp0) X(Kp1) X(Kp2) X(Kp3) X(Kp4)     \
    X(Kp5) X(Kp6) X(Kp7) X(Kp8) X(Kp9) X(KpDecimal) X(KpDivide) X(KpMultiply) X(KpSubtract) X(KpAdd) X(KpEnter)      \
    X(KpEqual) X(LeftShift) X(LeftControl) X(LeftAlt) X(LeftSuper) X(RightShift) X(RightControl) X(RightAlt)         \
    X(RightSuper) X(Menu)

enum class Key : u16 {
#define OX_INPUT_ENUM_ENTRY(name) name,
    OX_INPUT_KEYS(OX_INPUT_ENUM_ENTRY)
#undef OX_INPUT_ENUM_ENTRY
        Count
};
inline constexpr usize kKeyCount = static_cast<usize>(Key::Count);

enum class MouseButton : u8 { Left, Right, Middle, Button4, Button5, Count };
inline constexpr usize kMouseButtonCount = static_cast<usize>(MouseButton::Count);

// GLFW gamepad layout (Xbox naming).
enum class GamepadButton : u8 {
    A, B, X, Y, LeftBumper, RightBumper, Back, Start, Guide, LeftThumb, RightThumb, DpadUp, DpadRight, DpadDown,
    DpadLeft, Count
};
inline constexpr usize kGamepadButtonCount = static_cast<usize>(GamepadButton::Count);
// Sticks in [-1,1] with +Y = up (platform layers flip GLFW's Y); triggers in [0,1].
enum class GamepadAxis : u8 { LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger, Count };
inline constexpr usize kGamepadAxisCount = static_cast<usize>(GamepadAxis::Count);
inline constexpr u32 kMaxGamepads = 4;

[[nodiscard]] std::string_view keyName(Key key);
[[nodiscard]] std::optional<Key> keyFromName(std::string_view name);
[[nodiscard]] std::string_view mouseButtonName(MouseButton b);
[[nodiscard]] std::string_view gamepadButtonName(GamepadButton b);
[[nodiscard]] std::string_view gamepadAxisName(GamepadAxis a);

struct InputEvent {
    enum class Type : u8 {
        Key, MouseButton, MouseMove, MouseWheel, Text, GamepadButton, GamepadAxis, GamepadConnected,
        GamepadDisconnected, FocusLost
    };
    Type type = Type::Key;
    u16 code = 0;      // Key / MouseButton / GamepadButton / GamepadAxis
    u8 gamepad = 0;    // gamepad slot
    bool pressed = false;
    bool repeat = false;
    f32 value = 0.0f;  // gamepad axis
    glm::vec2 position{0.0f}; // MouseMove: cursor position in window pixels
    glm::vec2 delta{0.0f};    // MouseMove: raw motion (also while the cursor is locked); MouseWheel: scroll
    u32 codepoint = 0;        // Text

    static InputEvent key(Key k, bool pressed, bool repeat = false);
    static InputEvent mouseButton(MouseButton b, bool pressed);
    static InputEvent mouseMove(glm::vec2 position, glm::vec2 delta);
    static InputEvent mouseWheel(glm::vec2 scroll);
    static InputEvent text(u32 codepoint);
    static InputEvent gamepadButton(u32 pad, GamepadButton b, bool pressed);
    static InputEvent gamepadAxis(u32 pad, GamepadAxis a, f32 value);
    static InputEvent gamepadConnected(u32 pad, bool connected);
    static InputEvent focusLost();
};

// ---- mapping data (reflected; lives in .oxproj "input" and can be edited/saved) --------------------------------

enum class InputValueType : u8 { Bool, Axis1D, Axis2D, Axis3D };
enum class InputAccumulation : u8 { HighestAbsolute, Cumulative };
enum class InputModifierType : u8 { DeadZone, Negate, Swizzle, Scale };
enum class DeadZoneKind : u8 { Radial, Axial };
enum class SwizzleOrder : u8 { YXZ, ZYX, XZY, YZX, ZXY };
enum class InputTriggerType : u8 { Down, Pressed, Released, Hold, Tap, Chord };

struct InputModifier {
    InputModifierType type = InputModifierType::DeadZone;
    // DeadZone: magnitudes below `lower` -> 0, above `upper` -> 1, remapped linearly in between.
    DeadZoneKind deadZone = DeadZoneKind::Radial;
    f32 lower = 0.2f;
    f32 upper = 1.0f;
    // Negate: components with non-zero entries are negated. Scale: per-component factors.
    glm::vec3 vector{1.0f, 1.0f, 1.0f};
    SwizzleOrder order = SwizzleOrder::YXZ;

    static InputModifier makeDeadZone(f32 lower, f32 upper = 1.0f, DeadZoneKind kind = DeadZoneKind::Radial);
    static InputModifier makeNegate(bool x = true, bool y = true, bool z = true);
    static InputModifier makeSwizzle(SwizzleOrder order = SwizzleOrder::YXZ);
    static InputModifier makeScale(glm::vec3 factors);
};

struct InputTrigger {
    InputTriggerType type = InputTriggerType::Down;
    f32 threshold = 0.5f; // actuation threshold on the modified value's magnitude
    f32 time = 0.5f;      // Hold: hold time; Tap: max press time
    bool oneShot = true;  // Hold: trigger once per press (false = every frame after the hold time)
    std::string action;   // Chord: action that must be Triggered at the same time

    static InputTrigger down(f32 threshold = 0.5f);
    static InputTrigger pressed(f32 threshold = 0.5f);
    static InputTrigger released(f32 threshold = 0.5f);
    static InputTrigger hold(f32 seconds, bool oneShot = true);
    static InputTrigger tap(f32 maxSeconds = 0.2f);
    static InputTrigger chord(std::string action);
};

struct InputBinding {
    std::string action;
    std::string source; // see parseInputSource
    std::vector<InputModifier> modifiers;
    std::vector<InputTrigger> triggers; // none = Down with any non-zero actuation
    bool consume = true;                // hide the source from lower priority contexts
};

struct InputActionDesc {
    std::string name;
    InputValueType type = InputValueType::Bool;
    InputAccumulation accumulation = InputAccumulation::HighestAbsolute;
    std::string description;
};

struct InputContextDesc {
    std::string name;
    i32 priority = 0;
    std::vector<InputBinding> bindings;
};

struct InputMappingConfig {
    std::vector<InputActionDesc> actions;
    std::vector<InputContextDesc> contexts;
    std::vector<std::string> activeContexts; // activated on load with their priorities
};

// Reflection for the mapping types (idempotent).
void registerInputTypes();

// ---- sources ---------------------------------------------------------------------------------------------------

struct InputSource {
    enum class Device : u8 { None, Key, MouseButton, MouseAxis, GamepadButton, GamepadAxis, GamepadStick };
    // MouseAxis codes: 0 = X delta, 1 = Y delta, 2 = XY delta, 3 = wheel (vertical), 4 = wheel X.
    // GamepadStick codes: 0 = left, 1 = right.
    Device device = Device::None;
    u16 code = 0;

    [[nodiscard]] bool valid() const { return device != Device::None; }
    [[nodiscard]] u32 id() const { return (u32(device) << 16) | code; }
    friend bool operator==(const InputSource&, const InputSource&) = default;
};
// "Key.W", "Key.LeftShift", "Mouse.Left|Right|Middle|Button4|Button5", "Mouse.X|Y|XY|Wheel|WheelX",
// "Gamepad.A|B|X|Y|LeftBumper|...|DpadLeft", "Gamepad.LeftX|LeftY|RightX|RightY|LeftTrigger|RightTrigger",
// "Gamepad.LeftStick|RightStick". Gamepad sources read every connected pad (largest magnitude wins).
[[nodiscard]] std::optional<InputSource> parseInputSource(std::string_view name);
[[nodiscard]] std::string inputSourceName(InputSource source);

// ---- runtime state ----------------------------------------------------------------------------------------------

enum class TriggerState : u8 { None, Ongoing, Triggered };
enum class ActionEvent : u8 { Started, Ongoing, Triggered, Completed, Canceled };

struct ActionState {
    std::string name;
    InputValueType type = InputValueType::Bool;
    glm::vec3 value{0.0f};
    TriggerState state = TriggerState::None;
    TriggerState previous = TriggerState::None;
    f32 elapsed = 0.0f;       // seconds since Started (while not None)
    f32 triggeredTime = 0.0f; // seconds spent Triggered in the current activation
    // Edges of this frame.
    bool started = false;
    bool triggered = false; // state == Triggered
    bool completed = false;
    bool canceled = false;

    [[nodiscard]] bool active() const { return state != TriggerState::None; }
    [[nodiscard]] bool boolValue() const { return value.x != 0.0f || value.y != 0.0f || value.z != 0.0f; }
    [[nodiscard]] f32 axis1D() const { return value.x; }
    [[nodiscard]] glm::vec2 axis2D() const { return {value.x, value.y}; }
};

enum class CursorMode : u8 { Normal, Hidden, Locked };

class InputSystem {
public:
    using ActionCallback = std::function<void(const ActionState&)>;

    InputSystem();
    ~InputSystem();
    InputSystem(const InputSystem&) = delete;
    InputSystem& operator=(const InputSystem&) = delete;

    // ---- configuration (game thread) ----
    // Replaces actions and contexts, activates config.activeContexts. Rebinds are kept.
    void setMappings(const InputMappingConfig& config);
    [[nodiscard]] const InputMappingConfig& mappings() const { return m_config; }
    void addAction(InputActionDesc action);
    void addContext(InputContextDesc context); // replaces a context with the same name
    // Activates a known context (priority overrides the context's own when given).
    bool activateContext(std::string_view name, std::optional<i32> priority = std::nullopt);
    void deactivateContext(std::string_view name);
    [[nodiscard]] bool isContextActive(std::string_view name) const;
    [[nodiscard]] std::vector<std::string> activeContexts() const; // highest priority first

    // ---- events ----
    // Thread-safe; applied by the next update().
    void inject(const InputEvent& event);
    // Game thread, once per frame.
    void update(f64 dt);
    // Optional pre-filter run by update() (game thread) for every queued event before it is applied: return true
    // to consume it (UI layers on top of the game, e.g. ox::ui::UiSystem). Release events should not be consumed.
    using EventFilter = std::function<bool(const InputEvent&)>;
    void setEventFilter(EventFilter filter) { m_filter = std::move(filter); }

    // ---- raw state (as of the last update) ----
    [[nodiscard]] bool keyDown(Key k) const;
    [[nodiscard]] bool keyPressed(Key k) const;  // went down this frame
    [[nodiscard]] bool keyReleased(Key k) const; // went up this frame
    [[nodiscard]] bool mouseDown(MouseButton b) const;
    [[nodiscard]] bool mousePressed(MouseButton b) const;
    [[nodiscard]] bool mouseReleased(MouseButton b) const;
    [[nodiscard]] glm::vec2 mousePosition() const { return m_mousePos; }
    [[nodiscard]] glm::vec2 mouseDelta() const { return m_mouseDelta; }
    [[nodiscard]] glm::vec2 mouseWheel() const { return m_wheel; }
    [[nodiscard]] bool gamepadConnected(u32 pad) const;
    [[nodiscard]] bool gamepadDown(u32 pad, GamepadButton b) const;
    [[nodiscard]] f32 gamepadAxis(u32 pad, GamepadAxis a) const;
    [[nodiscard]] const std::u32string& textInput() const { return m_text; }
    // Raw value of a source (before modifiers): keys/buttons 0/1, axes, deltas.
    [[nodiscard]] glm::vec3 sourceValue(InputSource source) const;

    // ---- actions ----
    [[nodiscard]] const ActionState* action(std::string_view name) const;
    [[nodiscard]] bool triggered(std::string_view name) const;
    [[nodiscard]] bool started(std::string_view name) const;
    [[nodiscard]] bool completed(std::string_view name) const;
    [[nodiscard]] f32 axis1D(std::string_view name) const;
    [[nodiscard]] glm::vec2 axis2D(std::string_view name) const;
    // Called from update() on the game thread.
    Connection bindAction(std::string_view action, ActionEvent event, ActionCallback fn);

    // ---- rebinding ----
    // Changes the source of the index-th binding of `action` in `context`. Persist with rebinds()/applyRebinds()
    // (UserSettings::inputRebinds).
    bool rebind(std::string_view context, std::string_view action, u32 bindingIndex, std::string_view source);
    void resetBinding(std::string_view context, std::string_view action, u32 bindingIndex);
    void resetAllBindings();
    // "context/action/index" -> source name.
    [[nodiscard]] const std::map<std::string, std::string>& rebinds() const { return m_rebinds; }
    void applyRebinds(const std::map<std::string, std::string>& rebinds);
    // Effective source of a binding (after rebinding).
    [[nodiscard]] std::string bindingSource(std::string_view context, std::string_view action, u32 bindingIndex) const;
    // Calls fn with the next pressed key/button or deflected axis (rebinding UI). Consumes that input.
    void captureNextInput(std::function<void(const std::string& source)> fn);
    void cancelCapture();
    [[nodiscard]] bool capturing() const { return static_cast<bool>(m_capture); }

    // ---- cursor ----
    void setCursorMode(CursorMode mode);
    [[nodiscard]] CursorMode cursorMode() const { return m_cursorMode; }
    Signal<CursorMode> cursorModeChanged; // platform layers apply it
    Signal<> rebindsChanged;

private:
    struct TriggerRuntime {
        f32 heldTime = 0.0f;
        bool prevActuated = false;
        bool fired = false;
    };
    struct BindingRuntime {
        InputSource source;
        std::vector<TriggerRuntime> triggers;
        TriggerState state = TriggerState::None; // before chord gating
        glm::vec3 value{0.0f};
        u32 actionLocalIndex = 0;
    };
    struct ContextRuntime {
        std::string name;
        i32 priority = 0;
        u64 activationOrder = 0;
        std::vector<BindingRuntime> bindings;
    };
    struct GamepadState {
        bool connected = false;
        std::bitset<kGamepadButtonCount> down, pressed, released;
        std::array<f32, kGamepadAxisCount> axes{};
    };

    void applyEvent(const InputEvent& e);
    void rebuildContext(ContextRuntime& rt);
    void rebuildAllContexts();
    [[nodiscard]] const InputContextDesc* findContextDesc(std::string_view name) const;
    [[nodiscard]] InputActionDesc* findActionDesc(std::string_view name);
    bool processCapture(const std::vector<InputEvent>& events);
    void evaluateActions(f32 dt);
    TriggerState evaluateTriggers(const InputBinding& binding, BindingRuntime& rt, f32 dt);
    ActionState& stateFor(const InputActionDesc& desc);
    static std::string rebindKey(std::string_view context, std::string_view action, u32 index);

    std::mutex m_queueMutex;
    std::vector<InputEvent> m_queue;

    InputMappingConfig m_config;
    std::vector<ContextRuntime> m_active; // sorted: priority desc, activation order asc
    u64 m_activationCounter = 0;
    std::map<std::string, std::string> m_rebinds;
    std::unordered_map<std::string, ActionState> m_actions;
    struct CallbackSet {
        Signal<const ActionState&> signals[5];
    };
    std::unordered_map<std::string, std::unique_ptr<CallbackSet>> m_callbacks;
    std::function<void(const std::string&)> m_capture;
    EventFilter m_filter;

    std::bitset<kKeyCount> m_keyDown, m_keyPressed, m_keyReleased;
    std::bitset<kMouseButtonCount> m_mouseDown, m_mousePressed, m_mouseReleased;
    glm::vec2 m_mousePos{0.0f}, m_mouseDelta{0.0f}, m_wheel{0.0f};
    std::array<GamepadState, kMaxGamepads> m_pads{};
    std::u32string m_text;
    CursorMode m_cursorMode = CursorMode::Normal;
};

} // namespace ox
