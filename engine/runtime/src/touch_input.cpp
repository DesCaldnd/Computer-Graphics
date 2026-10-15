#include <oxwald/runtime/touch_input.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace ox {

TouchLayout TouchLayout::defaults() {
    TouchLayout l;
    l.buttons.push_back({"Jump", {0.84f, 0.70f, 0.98f, 0.95f}, {"Gamepad.A", "Key.Space"}});
    l.buttons.push_back({"Interact", {0.68f, 0.76f, 0.82f, 0.97f}, {"Gamepad.X", "Key.E"}});
    return l;
}

TouchInputMapper::TouchInputMapper(TouchLayout layout) { setLayout(std::move(layout)); }

void TouchInputMapper::setLayout(TouchLayout layout) {
    m_layout = std::move(layout);
    m_layout.gamepadSlot = std::min(m_layout.gamepadSlot, kMaxGamepads - 1);
    m_buttonSources.clear();
    for (const TouchButton& b : m_layout.buttons) {
        std::vector<InputSource> sources;
        for (const std::string& name : b.sources) {
            if (auto s = parseInputSource(name)) sources.push_back(*s);
        }
        m_buttonSources.push_back(std::move(sources));
    }
}

glm::vec2 TouchInputMapper::normalized(glm::vec2 px) const {
    return {m_viewport.x > 0 ? px.x / m_viewport.x : 0.0f, m_viewport.y > 0 ? px.y / m_viewport.y : 0.0f};
}

bool TouchInputMapper::inside(glm::vec4 r, glm::vec2 n) { return n.x >= r.x && n.x <= r.z && n.y >= r.y && n.y <= r.w; }

TouchInputMapper::Pointer* TouchInputMapper::find(i32 id) {
    for (Pointer& p : m_pointers) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

bool TouchInputMapper::stickActive() const {
    return std::any_of(m_pointers.begin(), m_pointers.end(), [](const Pointer& p) { return p.role == Role::Stick; });
}

void TouchInputMapper::ensurePad(std::vector<InputEvent>& out) {
    if (m_padAnnounced) return;
    m_padAnnounced = true;
    out.push_back(InputEvent::gamepadConnected(m_layout.gamepadSlot, true));
}

void TouchInputMapper::emitStick(glm::vec2 value, std::vector<InputEvent>& out) {
    ensurePad(out);
    m_stickValue = value;
    out.push_back(InputEvent::gamepadAxis(m_layout.gamepadSlot, GamepadAxis::LeftX, value.x));
    out.push_back(InputEvent::gamepadAxis(m_layout.gamepadSlot, GamepadAxis::LeftY, value.y));
}

void TouchInputMapper::emitButton(usize index, bool pressed, std::vector<InputEvent>& out) {
    if (index >= m_buttonSources.size()) return;
    for (const InputSource& s : m_buttonSources[index]) {
        switch (s.device) {
        case InputSource::Device::Key: out.push_back(InputEvent::key(Key(s.code), pressed)); break;
        case InputSource::Device::MouseButton: out.push_back(InputEvent::mouseButton(MouseButton(s.code), pressed)); break;
        case InputSource::Device::GamepadButton:
            ensurePad(out);
            out.push_back(InputEvent::gamepadButton(m_layout.gamepadSlot, GamepadButton(s.code), pressed));
            break;
        case InputSource::Device::GamepadAxis:
            ensurePad(out);
            out.push_back(InputEvent::gamepadAxis(m_layout.gamepadSlot, GamepadAxis(s.code), pressed ? 1.0f : 0.0f));
            break;
        default: break; // mouse axes / sticks make no sense as buttons
        }
    }
}

f32 TouchInputMapper::pinchDistance() const {
    const Pointer* a = nullptr;
    const Pointer* b = nullptr;
    for (const Pointer& p : m_pointers) {
        if (p.role != Role::Look) continue;
        if (!a) a = &p;
        else if (!b) b = &p;
    }
    return a && b ? glm::length(a->last - b->last) : -1.0f;
}

void TouchInputMapper::process(const TouchEvent& e, std::vector<InputEvent>& out) {
    const f32 height = std::max(m_viewport.y, 1.0f);
    switch (e.phase) {
    case TouchPhase::Down: {
        if (find(e.pointerId)) return; // duplicate down: ignore
        Pointer p;
        p.id = e.pointerId;
        p.origin = p.last = e.position;
        p.downTime = e.time;
        const glm::vec2 n = normalized(e.position);
        bool assigned = false;
        for (usize i = 0; i < m_layout.buttons.size(); ++i) {
            if (inside(m_layout.buttons[i].rect, n)) {
                p.role = Role::Button;
                p.button = i;
                emitButton(i, true, out);
                assigned = true;
                break;
            }
        }
        if (!assigned && inside(m_layout.moveZone, n) && !stickActive()) {
            p.role = Role::Stick;
            emitStick({0.0f, 0.0f}, out);
            assigned = true;
        }
        if (!assigned) {
            p.role = Role::Look;
            out.push_back(InputEvent::mouseMove(e.position, {0.0f, 0.0f})); // cursor position for UI hover / clicks
        }
        m_pointers.push_back(p);
        if (p.role == Role::Look) {
            const f32 d = pinchDistance();
            m_pinch = d >= 0.0f ? std::optional<f32>(d) : std::nullopt;
        }
        return;
    }
    case TouchPhase::Move: {
        Pointer* p = find(e.pointerId);
        if (!p) return;
        const glm::vec2 delta = e.position - p->last;
        p->last = e.position;
        p->travelled = std::max(p->travelled, glm::length(e.position - p->origin));
        if (p->role == Role::Stick) {
            const f32 radius = std::max(m_layout.stickRadius * height, 1.0f);
            glm::vec2 v = (e.position - p->origin) / radius;
            v.y = -v.y; // screen y down -> stick +Y up
            const f32 len = glm::length(v);
            if (len > 1.0f) v /= len;
            if (len < m_layout.stickDeadZone) v = {0.0f, 0.0f};
            emitStick(v, out);
        } else if (p->role == Role::Look) {
            if (m_pinch) {
                const f32 d = pinchDistance();
                if (d >= 0.0f) {
                    const f32 change = d - *m_pinch;
                    if (std::abs(change) >= 1.0f) {
                        out.push_back(InputEvent::mouseWheel({0.0f, change * m_layout.pinchWheelPerPixel}));
                        m_pinch = d;
                    }
                }
                return; // two fingers: zoom, not look
            }
            out.push_back(InputEvent::mouseMove(e.position, delta * m_layout.lookSensitivity));
        }
        return;
    }
    case TouchPhase::Up:
    case TouchPhase::Cancel: {
        auto it = std::find_if(m_pointers.begin(), m_pointers.end(), [&](const Pointer& p) { return p.id == e.pointerId; });
        if (it == m_pointers.end()) return;
        const Pointer p = *it;
        m_pointers.erase(it);
        if (p.role == Role::Stick) {
            emitStick({0.0f, 0.0f}, out);
        } else if (p.role == Role::Button) {
            emitButton(p.button, false, out);
        } else {
            const bool wasPinching = m_pinch.has_value();
            const f32 d = pinchDistance();
            m_pinch = d >= 0.0f ? std::optional<f32>(d) : std::nullopt;
            const bool tap = e.phase == TouchPhase::Up && !wasPinching && e.time - p.downTime <= m_layout.tapMaxSeconds &&
                             p.travelled <= m_layout.tapMaxDistance * height;
            if (tap) {
                out.push_back(InputEvent::mouseMove(p.origin, {0.0f, 0.0f}));
                out.push_back(InputEvent::mouseButton(MouseButton::Left, true));
                out.push_back(InputEvent::mouseButton(MouseButton::Left, false));
            }
        }
        return;
    }
    }
}

void TouchInputMapper::reset(std::vector<InputEvent>& out) {
    for (const Pointer& p : m_pointers) {
        if (p.role == Role::Button) emitButton(p.button, false, out);
    }
    if (stickActive() || m_stickValue != glm::vec2(0.0f)) emitStick({0.0f, 0.0f}, out);
    m_pointers.clear();
    m_pinch.reset();
}

} // namespace ox
