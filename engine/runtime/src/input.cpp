#include <oxwald/core/log.hpp>
#include <oxwald/core/profile.hpp>
#include <oxwald/core/reflect.hpp>
#include <oxwald/runtime/input.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace ox {

namespace {

constexpr std::string_view kKeyNames[] = {
#define OX_INPUT_NAME_ENTRY(name) #name,
    OX_INPUT_KEYS(OX_INPUT_NAME_ENTRY)
#undef OX_INPUT_NAME_ENTRY
};
static_assert(std::size(kKeyNames) == kKeyCount);

constexpr std::string_view kMouseButtonNames[] = {"Left", "Right", "Middle", "Button4", "Button5"};
constexpr std::string_view kGamepadButtonNames[] = {"A",         "B",          "X",          "Y",
                                                    "LeftBumper", "RightBumper", "Back",       "Start",
                                                    "Guide",     "LeftThumb",  "RightThumb", "DpadUp",
                                                    "DpadRight", "DpadDown",   "DpadLeft"};
constexpr std::string_view kGamepadAxisNames[] = {"LeftX", "LeftY", "RightX", "RightY", "LeftTrigger", "RightTrigger"};
constexpr std::string_view kMouseAxisNames[] = {"X", "Y", "XY", "Wheel", "WheelX"};
constexpr std::string_view kStickNames[] = {"LeftStick", "RightStick"};
static_assert(std::size(kGamepadButtonNames) == kGamepadButtonCount);
static_assert(std::size(kGamepadAxisNames) == kGamepadAxisCount);

template <usize N>
std::optional<u16> indexOf(const std::string_view (&names)[N], std::string_view name) {
    for (usize i = 0; i < N; ++i) {
        if (names[i] == name) return static_cast<u16>(i);
    }
    return std::nullopt;
}

constexpr f32 kImplicitActuation = 1e-4f;

TriggerState maxState(TriggerState a, TriggerState b) { return a > b ? a : b; }

glm::vec3 applyModifier(const InputModifier& m, glm::vec3 v) {
    switch (m.type) {
    case InputModifierType::DeadZone: {
        const f32 range = std::max(m.upper - m.lower, 1e-6f);
        auto remap = [&](f32 mag) { return std::clamp((mag - m.lower) / range, 0.0f, 1.0f); };
        if (m.deadZone == DeadZoneKind::Radial) {
            const f32 mag = glm::length(v);
            if (mag <= m.lower || mag == 0.0f) return glm::vec3(0.0f);
            return v / mag * remap(mag);
        }
        for (int i = 0; i < 3; ++i) {
            const f32 a = std::abs(v[i]);
            v[i] = a <= m.lower ? 0.0f : std::copysign(remap(a), v[i]);
        }
        return v;
    }
    case InputModifierType::Negate:
        for (int i = 0; i < 3; ++i) {
            if (m.vector[i] != 0.0f) v[i] = -v[i];
        }
        return v;
    case InputModifierType::Swizzle:
        switch (m.order) {
        case SwizzleOrder::YXZ: return {v.y, v.x, v.z};
        case SwizzleOrder::ZYX: return {v.z, v.y, v.x};
        case SwizzleOrder::XZY: return {v.x, v.z, v.y};
        case SwizzleOrder::YZX: return {v.y, v.z, v.x};
        case SwizzleOrder::ZXY: return {v.z, v.x, v.y};
        }
        return v;
    case InputModifierType::Scale: return v * m.vector;
    }
    return v;
}

glm::vec3 shapeForType(glm::vec3 v, InputValueType type) {
    switch (type) {
    case InputValueType::Bool: return glm::vec3(glm::length(v) > 0.0f ? 1.0f : 0.0f, 0.0f, 0.0f);
    case InputValueType::Axis1D: return {v.x, 0.0f, 0.0f};
    case InputValueType::Axis2D: return {v.x, v.y, 0.0f};
    case InputValueType::Axis3D: return v;
    }
    return v;
}

} // namespace

// ---- names ----------------------------------------------------------------------------------------------------

std::string_view keyName(Key key) {
    const auto i = static_cast<usize>(key);
    return i < kKeyCount ? kKeyNames[i] : std::string_view("Unknown");
}
std::optional<Key> keyFromName(std::string_view name) {
    if (auto i = indexOf(kKeyNames, name)) return static_cast<Key>(*i);
    return std::nullopt;
}
std::string_view mouseButtonName(MouseButton b) {
    const auto i = static_cast<usize>(b);
    return i < kMouseButtonCount ? kMouseButtonNames[i] : std::string_view("?");
}
std::string_view gamepadButtonName(GamepadButton b) {
    const auto i = static_cast<usize>(b);
    return i < kGamepadButtonCount ? kGamepadButtonNames[i] : std::string_view("?");
}
std::string_view gamepadAxisName(GamepadAxis a) {
    const auto i = static_cast<usize>(a);
    return i < kGamepadAxisCount ? kGamepadAxisNames[i] : std::string_view("?");
}

std::optional<InputSource> parseInputSource(std::string_view name) {
    const auto dot = name.find('.');
    if (dot == std::string_view::npos) return std::nullopt;
    const std::string_view dev = name.substr(0, dot);
    const std::string_view item = name.substr(dot + 1);
    using D = InputSource::Device;
    if (dev == "Key") {
        if (auto k = keyFromName(item); k && *k != Key::Unknown) return InputSource{D::Key, u16(*k)};
    } else if (dev == "Mouse") {
        if (auto b = indexOf(kMouseButtonNames, item)) return InputSource{D::MouseButton, *b};
        if (auto a = indexOf(kMouseAxisNames, item)) return InputSource{D::MouseAxis, *a};
    } else if (dev == "Gamepad") {
        if (auto b = indexOf(kGamepadButtonNames, item)) return InputSource{D::GamepadButton, *b};
        if (auto a = indexOf(kGamepadAxisNames, item)) return InputSource{D::GamepadAxis, *a};
        if (auto s = indexOf(kStickNames, item)) return InputSource{D::GamepadStick, *s};
    }
    return std::nullopt;
}

std::string inputSourceName(InputSource s) {
    using D = InputSource::Device;
    auto join = [](std::string_view a, std::string_view b) { return std::string(a) + "." + std::string(b); };
    switch (s.device) {
    case D::Key: return join("Key", keyName(Key(s.code)));
    case D::MouseButton: return s.code < std::size(kMouseButtonNames) ? join("Mouse", kMouseButtonNames[s.code]) : "";
    case D::MouseAxis: return s.code < std::size(kMouseAxisNames) ? join("Mouse", kMouseAxisNames[s.code]) : "";
    case D::GamepadButton:
        return s.code < kGamepadButtonCount ? join("Gamepad", kGamepadButtonNames[s.code]) : "";
    case D::GamepadAxis: return s.code < kGamepadAxisCount ? join("Gamepad", kGamepadAxisNames[s.code]) : "";
    case D::GamepadStick: return s.code < std::size(kStickNames) ? join("Gamepad", kStickNames[s.code]) : "";
    case D::None: break;
    }
    return {};
}

// ---- events / data helpers -----------------------------------------------------------------------------------

InputEvent InputEvent::key(Key k, bool pressed, bool repeat) {
    InputEvent e;
    e.type = Type::Key;
    e.code = u16(k);
    e.pressed = pressed;
    e.repeat = repeat;
    return e;
}
InputEvent InputEvent::mouseButton(MouseButton b, bool pressed) {
    InputEvent e;
    e.type = Type::MouseButton;
    e.code = u16(b);
    e.pressed = pressed;
    return e;
}
InputEvent InputEvent::mouseMove(glm::vec2 position, glm::vec2 delta) {
    InputEvent e;
    e.type = Type::MouseMove;
    e.position = position;
    e.delta = delta;
    return e;
}
InputEvent InputEvent::mouseWheel(glm::vec2 scroll) {
    InputEvent e;
    e.type = Type::MouseWheel;
    e.delta = scroll;
    return e;
}
InputEvent InputEvent::text(u32 codepoint) {
    InputEvent e;
    e.type = Type::Text;
    e.codepoint = codepoint;
    return e;
}
InputEvent InputEvent::gamepadButton(u32 pad, GamepadButton b, bool pressed) {
    InputEvent e;
    e.type = Type::GamepadButton;
    e.gamepad = u8(pad);
    e.code = u16(b);
    e.pressed = pressed;
    return e;
}
InputEvent InputEvent::gamepadAxis(u32 pad, GamepadAxis a, f32 value) {
    InputEvent e;
    e.type = Type::GamepadAxis;
    e.gamepad = u8(pad);
    e.code = u16(a);
    e.value = value;
    return e;
}
InputEvent InputEvent::gamepadConnected(u32 pad, bool connected) {
    InputEvent e;
    e.type = connected ? Type::GamepadConnected : Type::GamepadDisconnected;
    e.gamepad = u8(pad);
    return e;
}
InputEvent InputEvent::focusLost() {
    InputEvent e;
    e.type = Type::FocusLost;
    return e;
}

InputModifier InputModifier::makeDeadZone(f32 lower, f32 upper, DeadZoneKind kind) {
    InputModifier m;
    m.type = InputModifierType::DeadZone;
    m.lower = lower;
    m.upper = upper;
    m.deadZone = kind;
    return m;
}
InputModifier InputModifier::makeNegate(bool x, bool y, bool z) {
    InputModifier m;
    m.type = InputModifierType::Negate;
    m.vector = {x ? 1.0f : 0.0f, y ? 1.0f : 0.0f, z ? 1.0f : 0.0f};
    return m;
}
InputModifier InputModifier::makeSwizzle(SwizzleOrder order) {
    InputModifier m;
    m.type = InputModifierType::Swizzle;
    m.order = order;
    return m;
}
InputModifier InputModifier::makeScale(glm::vec3 factors) {
    InputModifier m;
    m.type = InputModifierType::Scale;
    m.vector = factors;
    return m;
}

InputTrigger InputTrigger::down(f32 threshold) { return {InputTriggerType::Down, threshold}; }
InputTrigger InputTrigger::pressed(f32 threshold) { return {InputTriggerType::Pressed, threshold}; }
InputTrigger InputTrigger::released(f32 threshold) { return {InputTriggerType::Released, threshold}; }
InputTrigger InputTrigger::hold(f32 seconds, bool oneShot) {
    InputTrigger t{InputTriggerType::Hold};
    t.time = seconds;
    t.oneShot = oneShot;
    return t;
}
InputTrigger InputTrigger::tap(f32 maxSeconds) {
    InputTrigger t{InputTriggerType::Tap};
    t.time = maxSeconds;
    return t;
}
InputTrigger InputTrigger::chord(std::string action) {
    InputTrigger t{InputTriggerType::Chord};
    t.action = std::move(action);
    return t;
}

void registerInputTypes() {
    using namespace ox::attr;
    OX_REFLECT_ENUM(InputValueType, "InputValueType")
        .value("Bool", InputValueType::Bool)
        .value("Axis1D", InputValueType::Axis1D)
        .value("Axis2D", InputValueType::Axis2D)
        .value("Axis3D", InputValueType::Axis3D);
    OX_REFLECT_ENUM(InputAccumulation, "InputAccumulation")
        .value("HighestAbsolute", InputAccumulation::HighestAbsolute)
        .value("Cumulative", InputAccumulation::Cumulative);
    OX_REFLECT_ENUM(InputModifierType, "InputModifierType")
        .value("DeadZone", InputModifierType::DeadZone)
        .value("Negate", InputModifierType::Negate)
        .value("Swizzle", InputModifierType::Swizzle)
        .value("Scale", InputModifierType::Scale);
    OX_REFLECT_ENUM(DeadZoneKind, "DeadZoneKind").value("Radial", DeadZoneKind::Radial).value("Axial", DeadZoneKind::Axial);
    OX_REFLECT_ENUM(SwizzleOrder, "SwizzleOrder")
        .value("YXZ", SwizzleOrder::YXZ)
        .value("ZYX", SwizzleOrder::ZYX)
        .value("XZY", SwizzleOrder::XZY)
        .value("YZX", SwizzleOrder::YZX)
        .value("ZXY", SwizzleOrder::ZXY);
    OX_REFLECT_ENUM(InputTriggerType, "InputTriggerType")
        .value("Down", InputTriggerType::Down)
        .value("Pressed", InputTriggerType::Pressed)
        .value("Released", InputTriggerType::Released)
        .value("Hold", InputTriggerType::Hold)
        .value("Tap", InputTriggerType::Tap)
        .value("Chord", InputTriggerType::Chord);
    OX_REFLECT_TYPE(InputModifier, "InputModifier")
        .field("type", &InputModifier::type)
        .field("deadZone", &InputModifier::deadZone)
        .field("lower", &InputModifier::lower)
        .field("upper", &InputModifier::upper)
        .field("vector", &InputModifier::vector)
        .field("order", &InputModifier::order);
    OX_REFLECT_TYPE(InputTrigger, "InputTrigger")
        .field("type", &InputTrigger::type)
        .field("threshold", &InputTrigger::threshold)
        .field("time", &InputTrigger::time)
        .field("oneShot", &InputTrigger::oneShot)
        .field("action", &InputTrigger::action);
    OX_REFLECT_TYPE(InputBinding, "InputBinding")
        .field("action", &InputBinding::action)
        .field("source", &InputBinding::source)
        .field("modifiers", &InputBinding::modifiers)
        .field("triggers", &InputBinding::triggers)
        .field("consume", &InputBinding::consume);
    OX_REFLECT_TYPE(InputActionDesc, "InputAction")
        .field("name", &InputActionDesc::name)
        .field("type", &InputActionDesc::type)
        .field("accumulation", &InputActionDesc::accumulation)
        .field("description", &InputActionDesc::description);
    OX_REFLECT_TYPE(InputContextDesc, "InputContext")
        .field("name", &InputContextDesc::name)
        .field("priority", &InputContextDesc::priority)
        .field("bindings", &InputContextDesc::bindings);
    OX_REFLECT_TYPE(InputMappingConfig, "InputMappings")
        .field("actions", &InputMappingConfig::actions)
        .field("contexts", &InputMappingConfig::contexts)
        .field("activeContexts", &InputMappingConfig::activeContexts);
}

// ---- InputSystem ------------------------------------------------------------------------------------------------

InputSystem::InputSystem() = default;
InputSystem::~InputSystem() = default;

void InputSystem::setMappings(const InputMappingConfig& config) {
    m_config = config;
    m_active.clear();
    m_actions.clear();
    for (const auto& name : m_config.activeContexts) activateContext(name);
}

void InputSystem::addAction(InputActionDesc action) {
    if (InputActionDesc* existing = findActionDesc(action.name)) {
        *existing = std::move(action);
    } else {
        m_config.actions.push_back(std::move(action));
    }
}

void InputSystem::addContext(InputContextDesc context) {
    auto it = std::find_if(m_config.contexts.begin(), m_config.contexts.end(),
                           [&](const InputContextDesc& c) { return c.name == context.name; });
    if (it != m_config.contexts.end()) {
        *it = std::move(context);
        for (auto& rt : m_active) {
            if (rt.name == it->name) rebuildContext(rt);
        }
    } else {
        m_config.contexts.push_back(std::move(context));
    }
}

const InputContextDesc* InputSystem::findContextDesc(std::string_view name) const {
    for (const auto& c : m_config.contexts) {
        if (c.name == name) return &c;
    }
    return nullptr;
}

InputActionDesc* InputSystem::findActionDesc(std::string_view name) {
    for (auto& a : m_config.actions) {
        if (a.name == name) return &a;
    }
    return nullptr;
}

bool InputSystem::activateContext(std::string_view name, std::optional<i32> priority) {
    const InputContextDesc* desc = findContextDesc(name);
    if (!desc) {
        OX_LOG_WARN("input", "unknown input context '{}'", name);
        return false;
    }
    deactivateContext(name);
    ContextRuntime rt;
    rt.name = desc->name;
    rt.priority = priority.value_or(desc->priority);
    rt.activationOrder = m_activationCounter++;
    rebuildContext(rt);
    m_active.push_back(std::move(rt));
    std::stable_sort(m_active.begin(), m_active.end(), [](const ContextRuntime& a, const ContextRuntime& b) {
        if (a.priority != b.priority) return a.priority > b.priority;
        return a.activationOrder < b.activationOrder;
    });
    return true;
}

void InputSystem::deactivateContext(std::string_view name) {
    std::erase_if(m_active, [&](const ContextRuntime& c) { return c.name == name; });
}

bool InputSystem::isContextActive(std::string_view name) const {
    return std::any_of(m_active.begin(), m_active.end(), [&](const ContextRuntime& c) { return c.name == name; });
}

std::vector<std::string> InputSystem::activeContexts() const {
    std::vector<std::string> out;
    for (const auto& c : m_active) out.push_back(c.name);
    return out;
}

std::string InputSystem::rebindKey(std::string_view context, std::string_view action, u32 index) {
    return std::string(context) + "/" + std::string(action) + "/" + std::to_string(index);
}

void InputSystem::rebuildContext(ContextRuntime& rt) {
    const InputContextDesc* desc = findContextDesc(rt.name);
    rt.bindings.clear();
    if (!desc) return;
    std::unordered_map<std::string, u32> perAction;
    for (const auto& b : desc->bindings) {
        BindingRuntime br;
        br.actionLocalIndex = perAction[b.action]++;
        std::string source = b.source;
        if (auto it = m_rebinds.find(rebindKey(rt.name, b.action, br.actionLocalIndex)); it != m_rebinds.end()) {
            source = it->second;
        }
        if (auto s = parseInputSource(source)) {
            br.source = *s;
        } else if (!source.empty()) {
            OX_LOG_WARN("input", "context '{}': unknown input source '{}'", rt.name, source);
        }
        br.triggers.resize(b.triggers.size());
        rt.bindings.push_back(std::move(br));
    }
}

void InputSystem::rebuildAllContexts() {
    for (auto& rt : m_active) rebuildContext(rt);
}

bool InputSystem::rebind(std::string_view context, std::string_view action, u32 bindingIndex,
                         std::string_view source) {
    if (!parseInputSource(source)) return false;
    const InputContextDesc* desc = findContextDesc(context);
    if (!desc) return false;
    u32 count = 0;
    std::string defaultSource;
    for (const auto& b : desc->bindings) {
        if (b.action != action) continue;
        if (count++ == bindingIndex) defaultSource = b.source;
    }
    if (bindingIndex >= count) return false;
    const std::string key = rebindKey(context, action, bindingIndex);
    if (source == defaultSource) {
        m_rebinds.erase(key);
    } else {
        m_rebinds[key] = std::string(source);
    }
    rebuildAllContexts();
    rebindsChanged.emit();
    return true;
}

void InputSystem::resetBinding(std::string_view context, std::string_view action, u32 bindingIndex) {
    if (m_rebinds.erase(rebindKey(context, action, bindingIndex)) > 0) {
        rebuildAllContexts();
        rebindsChanged.emit();
    }
}

void InputSystem::resetAllBindings() {
    if (m_rebinds.empty()) return;
    m_rebinds.clear();
    rebuildAllContexts();
    rebindsChanged.emit();
}

void InputSystem::applyRebinds(const std::map<std::string, std::string>& rebinds) {
    m_rebinds.clear();
    for (const auto& [k, v] : rebinds) {
        if (parseInputSource(v)) {
            m_rebinds[k] = v;
        } else {
            OX_LOG_WARN("input", "ignoring rebind {} -> unknown source '{}'", k, v);
        }
    }
    rebuildAllContexts();
}

std::string InputSystem::bindingSource(std::string_view context, std::string_view action, u32 bindingIndex) const {
    if (auto it = m_rebinds.find(rebindKey(context, action, bindingIndex)); it != m_rebinds.end()) return it->second;
    if (const InputContextDesc* desc = findContextDesc(context)) {
        u32 count = 0;
        for (const auto& b : desc->bindings) {
            if (b.action == action && count++ == bindingIndex) return b.source;
        }
    }
    return {};
}

void InputSystem::captureNextInput(std::function<void(const std::string&)> fn) { m_capture = std::move(fn); }
void InputSystem::cancelCapture() { m_capture = nullptr; }

void InputSystem::setCursorMode(CursorMode mode) {
    if (mode == m_cursorMode) return;
    m_cursorMode = mode;
    cursorModeChanged.emit(mode);
}

void InputSystem::inject(const InputEvent& event) {
    std::lock_guard lock(m_queueMutex);
    m_queue.push_back(event);
}

void InputSystem::applyEvent(const InputEvent& e) {
    using T = InputEvent::Type;
    switch (e.type) {
    case T::Key:
        if (e.code >= kKeyCount || e.repeat) break;
        if (e.pressed && !m_keyDown[e.code]) {
            m_keyDown[e.code] = true;
            m_keyPressed[e.code] = true;
        } else if (!e.pressed && m_keyDown[e.code]) {
            m_keyDown[e.code] = false;
            m_keyReleased[e.code] = true;
        }
        break;
    case T::MouseButton:
        if (e.code >= kMouseButtonCount) break;
        if (e.pressed && !m_mouseDown[e.code]) {
            m_mouseDown[e.code] = true;
            m_mousePressed[e.code] = true;
        } else if (!e.pressed && m_mouseDown[e.code]) {
            m_mouseDown[e.code] = false;
            m_mouseReleased[e.code] = true;
        }
        break;
    case T::MouseMove:
        m_mousePos = e.position;
        m_mouseDelta += e.delta;
        break;
    case T::MouseWheel: m_wheel += e.delta; break;
    case T::Text: m_text.push_back(static_cast<char32_t>(e.codepoint)); break;
    case T::GamepadButton: {
        if (e.gamepad >= kMaxGamepads || e.code >= kGamepadButtonCount) break;
        auto& p = m_pads[e.gamepad];
        p.connected = true;
        if (e.pressed && !p.down[e.code]) {
            p.down[e.code] = true;
            p.pressed[e.code] = true;
        } else if (!e.pressed && p.down[e.code]) {
            p.down[e.code] = false;
            p.released[e.code] = true;
        }
        break;
    }
    case T::GamepadAxis:
        if (e.gamepad >= kMaxGamepads || e.code >= kGamepadAxisCount) break;
        m_pads[e.gamepad].connected = true;
        m_pads[e.gamepad].axes[e.code] = e.value;
        break;
    case T::GamepadConnected:
        if (e.gamepad < kMaxGamepads) m_pads[e.gamepad].connected = true;
        break;
    case T::GamepadDisconnected:
        if (e.gamepad < kMaxGamepads) m_pads[e.gamepad] = GamepadState{};
        break;
    case T::FocusLost:
        // Releases everything so keys do not stick while another window has focus.
        m_keyReleased |= m_keyDown;
        m_keyDown.reset();
        m_mouseReleased |= m_mouseDown;
        m_mouseDown.reset();
        break;
    }
}

bool InputSystem::processCapture(const std::vector<InputEvent>& events) {
    if (!m_capture) return false;
    using T = InputEvent::Type;
    using D = InputSource::Device;
    for (const auto& e : events) {
        std::optional<InputSource> src;
        if (e.type == T::Key && e.pressed && !e.repeat && e.code != u16(Key::Unknown)) src = InputSource{D::Key, e.code};
        else if (e.type == T::MouseButton && e.pressed) src = InputSource{D::MouseButton, e.code};
        else if (e.type == T::GamepadButton && e.pressed) src = InputSource{D::GamepadButton, e.code};
        else if (e.type == T::GamepadAxis && std::abs(e.value) > 0.5f) src = InputSource{D::GamepadAxis, e.code};
        else if (e.type == T::MouseWheel && e.delta.y != 0.0f) src = InputSource{D::MouseAxis, 3};
        if (src) {
            auto fn = std::move(m_capture);
            m_capture = nullptr;
            fn(inputSourceName(*src));
            return true;
        }
    }
    return false;
}

void InputSystem::update(f64 dt) {
    OX_PROFILE_ZONE();
    std::vector<InputEvent> events;
    {
        std::lock_guard lock(m_queueMutex);
        events.swap(m_queue);
    }
    m_keyPressed.reset();
    m_keyReleased.reset();
    m_mousePressed.reset();
    m_mouseReleased.reset();
    for (auto& p : m_pads) {
        p.pressed.reset();
        p.released.reset();
    }
    m_mouseDelta = glm::vec2(0.0f);
    m_wheel = glm::vec2(0.0f);
    m_text.clear();

    if (m_filter) std::erase_if(events, [this](const InputEvent& e) { return m_filter(e); });
    for (const auto& e : events) applyEvent(e);
    const bool captured = processCapture(events);
    if (captured) {
        // The captured input must not also fire gameplay actions.
        m_keyPressed.reset();
        m_mousePressed.reset();
        for (auto& p : m_pads) p.pressed.reset();
    }
    evaluateActions(static_cast<f32>(dt));
}

bool InputSystem::keyDown(Key k) const { return usize(k) < kKeyCount && m_keyDown[usize(k)]; }
bool InputSystem::keyPressed(Key k) const { return usize(k) < kKeyCount && m_keyPressed[usize(k)]; }
bool InputSystem::keyReleased(Key k) const { return usize(k) < kKeyCount && m_keyReleased[usize(k)]; }
bool InputSystem::mouseDown(MouseButton b) const { return usize(b) < kMouseButtonCount && m_mouseDown[usize(b)]; }
bool InputSystem::mousePressed(MouseButton b) const { return usize(b) < kMouseButtonCount && m_mousePressed[usize(b)]; }
bool InputSystem::mouseReleased(MouseButton b) const {
    return usize(b) < kMouseButtonCount && m_mouseReleased[usize(b)];
}
bool InputSystem::gamepadConnected(u32 pad) const { return pad < kMaxGamepads && m_pads[pad].connected; }
bool InputSystem::gamepadDown(u32 pad, GamepadButton b) const {
    return pad < kMaxGamepads && usize(b) < kGamepadButtonCount && m_pads[pad].down[usize(b)];
}
f32 InputSystem::gamepadAxis(u32 pad, GamepadAxis a) const {
    return pad < kMaxGamepads && usize(a) < kGamepadAxisCount ? m_pads[pad].axes[usize(a)] : 0.0f;
}

glm::vec3 InputSystem::sourceValue(InputSource s) const {
    using D = InputSource::Device;
    auto bestPad = [&](auto fn) {
        glm::vec3 best(0.0f);
        for (const auto& p : m_pads) {
            if (!p.connected) continue;
            const glm::vec3 v = fn(p);
            if (glm::length(v) > glm::length(best)) best = v;
        }
        return best;
    };
    switch (s.device) {
    case D::Key:
        // A press and release inside one frame still counts as actuated for that frame.
        return glm::vec3(s.code < kKeyCount && (m_keyDown[s.code] || m_keyPressed[s.code]) ? 1.0f : 0.0f, 0, 0);
    case D::MouseButton:
        return glm::vec3(s.code < kMouseButtonCount && (m_mouseDown[s.code] || m_mousePressed[s.code]) ? 1.0f : 0.0f,
                         0, 0);
    case D::MouseAxis:
        switch (s.code) {
        case 0: return {m_mouseDelta.x, 0, 0};
        case 1: return {m_mouseDelta.y, 0, 0};
        case 2: return {m_mouseDelta.x, m_mouseDelta.y, 0};
        case 3: return {m_wheel.y, 0, 0};
        case 4: return {m_wheel.x, 0, 0};
        default: return glm::vec3(0.0f);
        }
    case D::GamepadButton:
        if (s.code >= kGamepadButtonCount) return glm::vec3(0.0f);
        return bestPad([&](const GamepadState& p) {
            return glm::vec3(p.down[s.code] || p.pressed[s.code] ? 1.0f : 0.0f, 0, 0);
        });
    case D::GamepadAxis:
        if (s.code >= kGamepadAxisCount) return glm::vec3(0.0f);
        return bestPad([&](const GamepadState& p) { return glm::vec3(p.axes[s.code], 0, 0); });
    case D::GamepadStick: {
        const usize x = s.code == 0 ? usize(GamepadAxis::LeftX) : usize(GamepadAxis::RightX);
        return bestPad([&](const GamepadState& p) { return glm::vec3(p.axes[x], p.axes[x + 1], 0); });
    }
    case D::None: break;
    }
    return glm::vec3(0.0f);
}

TriggerState InputSystem::evaluateTriggers(const InputBinding& binding, BindingRuntime& rt, f32 dt) {
    const f32 mag = glm::length(rt.value);
    bool anyExplicit = false;
    TriggerState result = TriggerState::None;
    for (usize i = 0; i < binding.triggers.size() && i < rt.triggers.size(); ++i) {
        const InputTrigger& t = binding.triggers[i];
        TriggerRuntime& s = rt.triggers[i];
        if (t.type == InputTriggerType::Chord) continue; // implicit, gated later
        anyExplicit = true;
        const bool actuated = mag >= t.threshold;
        TriggerState st = TriggerState::None;
        switch (t.type) {
        case InputTriggerType::Down: st = actuated ? TriggerState::Triggered : TriggerState::None; break;
        case InputTriggerType::Pressed:
            st = actuated && !s.prevActuated ? TriggerState::Triggered : TriggerState::None;
            break;
        case InputTriggerType::Released:
            st = actuated ? TriggerState::Ongoing : (s.prevActuated ? TriggerState::Triggered : TriggerState::None);
            break;
        case InputTriggerType::Hold:
            if (actuated) {
                s.heldTime += dt;
                if (s.heldTime >= t.time) {
                    if (t.oneShot) {
                        st = s.fired ? TriggerState::None : TriggerState::Triggered;
                        s.fired = true;
                    } else {
                        st = TriggerState::Triggered;
                    }
                } else {
                    st = TriggerState::Ongoing;
                }
            } else {
                s.heldTime = 0.0f;
                s.fired = false;
            }
            break;
        case InputTriggerType::Tap:
            if (actuated) {
                if (!s.prevActuated) s.heldTime = 0.0f;
                s.heldTime += dt;
                st = s.heldTime <= t.time ? TriggerState::Ongoing : TriggerState::None;
            } else if (s.prevActuated) {
                st = s.heldTime <= t.time ? TriggerState::Triggered : TriggerState::None;
                s.heldTime = 0.0f;
            }
            break;
        case InputTriggerType::Chord: break;
        }
        s.prevActuated = actuated;
        result = maxState(result, st);
    }
    if (!anyExplicit) result = mag > kImplicitActuation ? TriggerState::Triggered : TriggerState::None;
    return result;
}

ActionState& InputSystem::stateFor(const InputActionDesc& desc) {
    auto [it, inserted] = m_actions.try_emplace(desc.name);
    if (inserted) {
        it->second.name = desc.name;
    }
    it->second.type = desc.type;
    return it->second;
}

void InputSystem::evaluateActions(f32 dt) {
    // 1. Binding values and trigger states (stateful triggers advance exactly once per frame).
    std::unordered_map<u32, bool> blocked;
    struct Contribution {
        const InputBinding* binding;
        BindingRuntime* rt;
    };
    std::unordered_map<std::string, std::vector<Contribution>> perAction;
    for (auto& ctx : m_active) {
        const InputContextDesc* desc = findContextDesc(ctx.name);
        if (!desc || desc->bindings.size() != ctx.bindings.size()) continue;
        std::vector<u32> consumed;
        for (usize i = 0; i < ctx.bindings.size(); ++i) {
            const InputBinding& b = desc->bindings[i];
            BindingRuntime& rt = ctx.bindings[i];
            glm::vec3 v(0.0f);
            if (rt.source.valid() && !blocked.contains(rt.source.id())) {
                v = sourceValue(rt.source);
                for (const auto& m : b.modifiers) v = applyModifier(m, v);
            }
            rt.value = v;
            rt.state = evaluateTriggers(b, rt, dt);
            if (b.consume && rt.source.valid()) consumed.push_back(rt.source.id());
            perAction[b.action].push_back({&b, &rt});
        }
        for (u32 id : consumed) blocked[id] = true;
    }

    // 2. Resolve actions; chord-gated bindings need the state of their chord action, so iterate until stable.
    std::unordered_map<std::string, TriggerState> resolved;
    bool force = false;
    auto chordsReady = [&](const InputBinding& b) {
        if (force) return true;
        for (const auto& t : b.triggers) {
            if (t.type == InputTriggerType::Chord && !t.action.empty() && t.action != b.action &&
                !resolved.contains(t.action) && perAction.contains(t.action)) {
                return false;
            }
        }
        return true;
    };
    auto chordsSatisfied = [&](const InputBinding& b) {
        for (const auto& t : b.triggers) {
            if (t.type != InputTriggerType::Chord) continue;
            auto it = resolved.find(t.action);
            if (it == resolved.end() || it->second != TriggerState::Triggered) return false;
        }
        return true;
    };

    std::vector<InputActionDesc*> pending;
    for (auto& a : m_config.actions) pending.push_back(&a);
    while (!pending.empty()) {
        bool progress = false;
        for (auto it = pending.begin(); it != pending.end();) {
            InputActionDesc* desc = *it;
            auto contribs = perAction.find(desc->name);
            bool ready = true;
            if (contribs != perAction.end()) {
                for (const auto& c : contribs->second) ready = ready && chordsReady(*c.binding);
            }
            if (!ready) {
                ++it;
                continue;
            }
            TriggerState state = TriggerState::None;
            glm::vec3 value(0.0f);
            if (contribs != perAction.end()) {
                for (const auto& c : contribs->second) {
                    TriggerState s = c.rt->state;
                    if (s != TriggerState::None && !chordsSatisfied(*c.binding)) s = TriggerState::None;
                    if (s == TriggerState::None) continue;
                    state = maxState(state, s);
                    if (desc->accumulation == InputAccumulation::Cumulative) {
                        value += c.rt->value;
                    } else {
                        for (int k = 0; k < 3; ++k) {
                            if (std::abs(c.rt->value[k]) > std::abs(value[k])) value[k] = c.rt->value[k];
                        }
                    }
                }
            }
            resolved[desc->name] = state;

            ActionState& as = stateFor(*desc);
            as.previous = as.state;
            as.state = state;
            as.value = state == TriggerState::None ? glm::vec3(0.0f) : shapeForType(value, desc->type);
            as.started = as.previous == TriggerState::None && state != TriggerState::None;
            as.triggered = state == TriggerState::Triggered;
            as.completed = as.previous == TriggerState::Triggered && state == TriggerState::None;
            as.canceled = as.previous == TriggerState::Ongoing && state == TriggerState::None;
            if (state == TriggerState::None) {
                as.elapsed = 0.0f;
                as.triggeredTime = 0.0f;
            } else {
                if (!as.started) as.elapsed += dt;
                if (as.triggered) as.triggeredTime += dt;
            }
            it = pending.erase(it);
            progress = true;
        }
        if (!progress) {
            // Chord cycle: resolve the rest; chords on unresolved actions count as not satisfied.
            force = true;
        }
    }

    // 3. Callbacks (after every action is resolved so handlers see a consistent frame).
    for (const auto& [name, st] : m_actions) {
        auto cb = m_callbacks.find(name);
        if (cb == m_callbacks.end()) continue;
        auto& sigs = cb->second->signals;
        if (st.started) sigs[usize(ActionEvent::Started)].emit(st);
        if (st.state == TriggerState::Ongoing) sigs[usize(ActionEvent::Ongoing)].emit(st);
        if (st.triggered) sigs[usize(ActionEvent::Triggered)].emit(st);
        if (st.completed) sigs[usize(ActionEvent::Completed)].emit(st);
        if (st.canceled) sigs[usize(ActionEvent::Canceled)].emit(st);
    }
}

const ActionState* InputSystem::action(std::string_view name) const {
    auto it = m_actions.find(std::string(name));
    return it != m_actions.end() ? &it->second : nullptr;
}
bool InputSystem::triggered(std::string_view name) const {
    const auto* a = action(name);
    return a && a->triggered;
}
bool InputSystem::started(std::string_view name) const {
    const auto* a = action(name);
    return a && a->started;
}
bool InputSystem::completed(std::string_view name) const {
    const auto* a = action(name);
    return a && a->completed;
}
f32 InputSystem::axis1D(std::string_view name) const {
    const auto* a = action(name);
    return a ? a->value.x : 0.0f;
}
glm::vec2 InputSystem::axis2D(std::string_view name) const {
    const auto* a = action(name);
    return a ? a->axis2D() : glm::vec2(0.0f);
}

Connection InputSystem::bindAction(std::string_view action, ActionEvent event, ActionCallback fn) {
    auto& set = m_callbacks[std::string(action)];
    if (!set) set = std::make_unique<CallbackSet>();
    return set->signals[usize(event)].connect([fn = std::move(fn)](const ActionState& s) { fn(s); });
}

} // namespace ox
