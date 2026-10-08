#include <oxwald/runtime/platform.hpp>

#if OX_HAS_GLFW

#include <oxwald/core/log.hpp>
#include <oxwald/core/profile.hpp>

#include <GLFW/glfw3.h>

#if OX_HAS_RHI_GLFW
#include <oxwald/rhi/glfw_surface.hpp>
#endif

#include <cmath>

namespace ox {

namespace {

bool g_glfwInitialized = false;
int g_windowCount = 0;

bool ensureGlfw() {
    if (g_glfwInitialized) return true;
#if OX_HAS_RHI_GLFW
    // Hands rhi's Vulkan loader (bundled MoltenVK on macOS) to GLFW; must precede glfwInit().
    if (!rhi::initGlfwVulkan()) OX_LOG_WARN("platform", "Vulkan loader unavailable; window will have no surface");
#endif
    glfwSetErrorCallback([](int code, const char* msg) { OX_LOG_ERROR("glfw", "error {}: {}", code, msg); });
    if (!glfwInit()) return false;
    g_glfwInitialized = true;
    return true;
}

GlfwPlatform* self(GLFWwindow* w) { return static_cast<GlfwPlatform*>(glfwGetWindowUserPointer(w)); }

GLFWmonitor* monitorAt(i32 index) {
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    if (!monitors || count == 0) return nullptr;
    return monitors[std::clamp(index, 0, count - 1)];
}

} // namespace

Key GlfwPlatform::translateKey(int k) {
    if (k >= GLFW_KEY_A && k <= GLFW_KEY_Z) return static_cast<Key>(u16(Key::A) + (k - GLFW_KEY_A));
    if (k >= GLFW_KEY_0 && k <= GLFW_KEY_9) return static_cast<Key>(u16(Key::Num0) + (k - GLFW_KEY_0));
    if (k >= GLFW_KEY_F1 && k <= GLFW_KEY_F12) return static_cast<Key>(u16(Key::F1) + (k - GLFW_KEY_F1));
    if (k >= GLFW_KEY_KP_0 && k <= GLFW_KEY_KP_9) return static_cast<Key>(u16(Key::Kp0) + (k - GLFW_KEY_KP_0));
    switch (k) {
    case GLFW_KEY_SPACE: return Key::Space;
    case GLFW_KEY_APOSTROPHE: return Key::Apostrophe;
    case GLFW_KEY_COMMA: return Key::Comma;
    case GLFW_KEY_MINUS: return Key::Minus;
    case GLFW_KEY_PERIOD: return Key::Period;
    case GLFW_KEY_SLASH: return Key::Slash;
    case GLFW_KEY_SEMICOLON: return Key::Semicolon;
    case GLFW_KEY_EQUAL: return Key::Equal;
    case GLFW_KEY_LEFT_BRACKET: return Key::LeftBracket;
    case GLFW_KEY_BACKSLASH: return Key::Backslash;
    case GLFW_KEY_RIGHT_BRACKET: return Key::RightBracket;
    case GLFW_KEY_GRAVE_ACCENT: return Key::GraveAccent;
    case GLFW_KEY_ESCAPE: return Key::Escape;
    case GLFW_KEY_ENTER: return Key::Enter;
    case GLFW_KEY_TAB: return Key::Tab;
    case GLFW_KEY_BACKSPACE: return Key::Backspace;
    case GLFW_KEY_INSERT: return Key::Insert;
    case GLFW_KEY_DELETE: return Key::Delete;
    case GLFW_KEY_RIGHT: return Key::Right;
    case GLFW_KEY_LEFT: return Key::Left;
    case GLFW_KEY_DOWN: return Key::Down;
    case GLFW_KEY_UP: return Key::Up;
    case GLFW_KEY_PAGE_UP: return Key::PageUp;
    case GLFW_KEY_PAGE_DOWN: return Key::PageDown;
    case GLFW_KEY_HOME: return Key::Home;
    case GLFW_KEY_END: return Key::End;
    case GLFW_KEY_CAPS_LOCK: return Key::CapsLock;
    case GLFW_KEY_SCROLL_LOCK: return Key::ScrollLock;
    case GLFW_KEY_NUM_LOCK: return Key::NumLock;
    case GLFW_KEY_PRINT_SCREEN: return Key::PrintScreen;
    case GLFW_KEY_PAUSE: return Key::Pause;
    case GLFW_KEY_KP_DECIMAL: return Key::KpDecimal;
    case GLFW_KEY_KP_DIVIDE: return Key::KpDivide;
    case GLFW_KEY_KP_MULTIPLY: return Key::KpMultiply;
    case GLFW_KEY_KP_SUBTRACT: return Key::KpSubtract;
    case GLFW_KEY_KP_ADD: return Key::KpAdd;
    case GLFW_KEY_KP_ENTER: return Key::KpEnter;
    case GLFW_KEY_KP_EQUAL: return Key::KpEqual;
    case GLFW_KEY_LEFT_SHIFT: return Key::LeftShift;
    case GLFW_KEY_LEFT_CONTROL: return Key::LeftControl;
    case GLFW_KEY_LEFT_ALT: return Key::LeftAlt;
    case GLFW_KEY_LEFT_SUPER: return Key::LeftSuper;
    case GLFW_KEY_RIGHT_SHIFT: return Key::RightShift;
    case GLFW_KEY_RIGHT_CONTROL: return Key::RightControl;
    case GLFW_KEY_RIGHT_ALT: return Key::RightAlt;
    case GLFW_KEY_RIGHT_SUPER: return Key::RightSuper;
    case GLFW_KEY_MENU: return Key::Menu;
    default: return Key::Unknown;
    }
}

Result<std::unique_ptr<GlfwPlatform>> GlfwPlatform::create(const WindowDesc& desc, InputSystem* input) {
    OX_PROFILE_ZONE();
    if (!ensureGlfw()) return makeError("glfwInit failed (no display?)");
    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, desc.resizable ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_VISIBLE, desc.visible ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    GLFWwindow* window = glfwCreateWindow(desc.size.x, desc.size.y, desc.title.c_str(), nullptr, nullptr);
    if (!window) return makeError("glfwCreateWindow failed");
    ++g_windowCount;

    std::unique_ptr<GlfwPlatform> p(new GlfwPlatform());
    p->m_window = window;
    p->m_input = input;
    p->m_windowedSize = desc.size;
    glfwGetWindowPos(window, &p->m_windowedPos.x, &p->m_windowedPos.y);
    glfwSetWindowUserPointer(window, p.get());
    installCallbacks(window);
    if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
#if OX_HAS_RHI_GLFW
    p->m_surfaceProvider = std::unique_ptr<void, void (*)(void*)>(
        new rhi::GlfwSurfaceProvider(window), [](void* q) { delete static_cast<rhi::GlfwSurfaceProvider*>(q); });
#endif
    if (desc.mode != WindowMode::Windowed) p->setWindowMode(desc.mode, {0, 0}, desc.monitor);
    p->setInput(input);
    return p;
}

GlfwPlatform::~GlfwPlatform() {
    m_surfaceProvider.reset();
    if (m_window) {
        glfwDestroyWindow(m_window);
        if (--g_windowCount == 0 && g_glfwInitialized) {
            glfwTerminate();
            g_glfwInitialized = false;
        }
    }
}

void GlfwPlatform::installCallbacks(GLFWwindow* window) {
    glfwSetKeyCallback(window, [](GLFWwindow* w, int key, int, int action, int) {
        auto* p = self(w);
        if (!p || !p->m_input) return;
        const Key k = translateKey(key);
        if (k == Key::Unknown) return;
        p->m_input->inject(InputEvent::key(k, action != GLFW_RELEASE, action == GLFW_REPEAT));
    });
    glfwSetCharCallback(window, [](GLFWwindow* w, unsigned int cp) {
        if (auto* p = self(w); p && p->m_input) p->m_input->inject(InputEvent::text(cp));
    });
    glfwSetMouseButtonCallback(window, [](GLFWwindow* w, int button, int action, int) {
        auto* p = self(w);
        if (!p || !p->m_input || button < 0 || button >= int(kMouseButtonCount)) return;
        p->m_input->inject(InputEvent::mouseButton(MouseButton(button), action == GLFW_PRESS));
    });
    glfwSetCursorPosCallback(window, [](GLFWwindow* w, double x, double y) {
        auto* p = self(w);
        if (!p || !p->m_input) return;
        const glm::dvec2 pos{x, y};
        const glm::dvec2 delta = p->m_hasCursor ? pos - p->m_lastCursor : glm::dvec2(0.0);
        p->m_lastCursor = pos;
        p->m_hasCursor = true;
        p->m_input->inject(InputEvent::mouseMove(glm::vec2(pos), glm::vec2(delta)));
    });
    glfwSetScrollCallback(window, [](GLFWwindow* w, double dx, double dy) {
        if (auto* p = self(w); p && p->m_input) p->m_input->inject(InputEvent::mouseWheel({f32(dx), f32(dy)}));
    });
    glfwSetWindowFocusCallback(window, [](GLFWwindow* w, int focused) {
        if (auto* p = self(w); p && p->m_input && !focused) p->m_input->inject(InputEvent::focusLost());
    });
    glfwSetCursorEnterCallback(window, [](GLFWwindow* w, int entered) {
        if (auto* p = self(w); p && !entered) p->m_hasCursor = false;
    });
}

void GlfwPlatform::pollEvents() {
    OX_PROFILE_ZONE();
    glfwPollEvents();
    pollGamepads();
}

void GlfwPlatform::pollGamepads() {
    if (!m_input) return;
    for (u32 pad = 0; pad < kMaxGamepads; ++pad) {
        const int jid = GLFW_JOYSTICK_1 + int(pad);
        PadState& s = m_pads[pad];
        GLFWgamepadstate st{};
        const bool connected = glfwJoystickIsGamepad(jid) && glfwGetGamepadState(jid, &st);
        if (connected != s.connected) {
            m_input->inject(InputEvent::gamepadConnected(pad, connected));
            s = PadState{};
            s.connected = connected;
        }
        if (!connected) continue;
        for (usize b = 0; b < kGamepadButtonCount; ++b) {
            const bool down = st.buttons[b] == GLFW_PRESS;
            if (down != s.buttons[b]) {
                s.buttons[b] = down;
                m_input->inject(InputEvent::gamepadButton(pad, GamepadButton(b), down));
            }
        }
        for (usize a = 0; a < kGamepadAxisCount; ++a) {
            f32 v = st.axes[a];
            // GLFW: stick Y is +down, triggers are [-1,1]. Engine: +up, triggers [0,1].
            if (a == usize(GamepadAxis::LeftY) || a == usize(GamepadAxis::RightY)) v = -v;
            if (a == usize(GamepadAxis::LeftTrigger) || a == usize(GamepadAxis::RightTrigger)) v = (v + 1.0f) * 0.5f;
            if (std::abs(v - s.axes[a]) > 1e-4f) {
                s.axes[a] = v;
                m_input->inject(InputEvent::gamepadAxis(pad, GamepadAxis(a), v));
            }
        }
    }
}

void GlfwPlatform::setInput(InputSystem* input) {
    m_input = input;
    m_cursorConnection.disconnect();
    if (input) {
        m_cursorConnection = input->cursorModeChanged.connect([this](CursorMode m) { setCursorMode(m); });
        setCursorMode(input->cursorMode());
    }
}

bool GlfwPlatform::shouldClose() const { return glfwWindowShouldClose(m_window) != 0; }
void GlfwPlatform::requestClose() { glfwSetWindowShouldClose(m_window, GLFW_TRUE); }

RenderSurface GlfwPlatform::surface() const {
    RenderSurface s;
#if OX_HAS_RHI_GLFW
    s.provider = static_cast<rhi::GlfwSurfaceProvider*>(m_surfaceProvider.get());
#endif
    s.nativeWindow = m_window;
    s.framebufferSize = framebufferSize();
    s.dpiScale = contentScale().x;
    return s;
}

glm::uvec2 GlfwPlatform::framebufferSize() const {
    int w = 0, h = 0;
    glfwGetFramebufferSize(m_window, &w, &h);
    return {u32(std::max(w, 0)), u32(std::max(h, 0))};
}

glm::vec2 GlfwPlatform::contentScale() const {
    glm::vec2 s{1.0f};
    glfwGetWindowContentScale(m_window, &s.x, &s.y);
    return s;
}

void GlfwPlatform::setWindowMode(WindowMode mode, glm::ivec2 resolution, i32 monitorIndex) {
    GLFWmonitor* monitor = monitorAt(monitorIndex);
    if (m_mode == WindowMode::Windowed) {
        glfwGetWindowPos(m_window, &m_windowedPos.x, &m_windowedPos.y);
        glfwGetWindowSize(m_window, &m_windowedSize.x, &m_windowedSize.y);
    }
    const GLFWvidmode* vm = monitor ? glfwGetVideoMode(monitor) : nullptr;
    switch (mode) {
    case WindowMode::Windowed: {
        const glm::ivec2 size = resolution.x > 0 && resolution.y > 0 ? resolution : m_windowedSize;
        glfwSetWindowMonitor(m_window, nullptr, m_windowedPos.x, m_windowedPos.y, size.x, size.y, GLFW_DONT_CARE);
        glfwSetWindowAttrib(m_window, GLFW_DECORATED, GLFW_TRUE);
        break;
    }
    case WindowMode::Borderless: {
        if (!monitor || !vm) break;
        int mx = 0, my = 0;
        glfwGetMonitorPos(monitor, &mx, &my);
        glfwSetWindowMonitor(m_window, nullptr, mx, my, vm->width, vm->height, GLFW_DONT_CARE);
        glfwSetWindowAttrib(m_window, GLFW_DECORATED, GLFW_FALSE);
        break;
    }
    case WindowMode::Fullscreen: {
        if (!monitor || !vm) break;
        const glm::ivec2 size = resolution.x > 0 && resolution.y > 0 ? resolution : glm::ivec2(vm->width, vm->height);
        glfwSetWindowMonitor(m_window, monitor, 0, 0, size.x, size.y, vm->refreshRate);
        break;
    }
    }
    m_mode = mode;
}

void GlfwPlatform::applyWindowSettings(const GraphicsSettings& g) {
    int w = 0, h = 0;
    glfwGetWindowSize(m_window, &w, &h);
    const bool sizeChanged = g.resolution.x > 0 && g.resolution.y > 0 && (g.resolution.x != w || g.resolution.y != h);
    if (g.windowMode != m_mode || (sizeChanged && g.windowMode != WindowMode::Borderless)) {
        setWindowMode(g.windowMode, g.resolution, g.monitor);
    }
}

void GlfwPlatform::setCursorMode(CursorMode mode) {
    const int glfwMode = mode == CursorMode::Normal ? GLFW_CURSOR_NORMAL
                         : mode == CursorMode::Hidden ? GLFW_CURSOR_HIDDEN
                                                      : GLFW_CURSOR_DISABLED;
    glfwSetInputMode(m_window, GLFW_CURSOR, glfwMode);
}

std::vector<MonitorInfo> GlfwPlatform::monitors() const {
    std::vector<MonitorInfo> out;
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    for (int i = 0; i < count; ++i) {
        MonitorInfo info;
        if (const char* name = glfwGetMonitorName(monitors[i])) info.name = name;
        glfwGetMonitorPos(monitors[i], &info.position.x, &info.position.y);
        glfwGetMonitorContentScale(monitors[i], &info.contentScale.x, &info.contentScale.y);
        if (const GLFWvidmode* vm = glfwGetVideoMode(monitors[i])) info.current = {{vm->width, vm->height}, vm->refreshRate};
        int modeCount = 0;
        const GLFWvidmode* modes = glfwGetVideoModes(monitors[i], &modeCount);
        for (int m = 0; m < modeCount; ++m) {
            VideoMode vm{{modes[m].width, modes[m].height}, modes[m].refreshRate};
            if (std::find(info.modes.begin(), info.modes.end(), vm) == info.modes.end()) info.modes.push_back(vm);
        }
        out.push_back(std::move(info));
    }
    return out;
}

void GlfwPlatform::setTitle(const std::string& title) { glfwSetWindowTitle(m_window, title.c_str()); }

std::string GlfwPlatform::clipboard() const {
    const char* text = glfwGetClipboardString(m_window);
    return text ? std::string(text) : std::string();
}

void GlfwPlatform::setClipboard(const std::string& text) { glfwSetClipboardString(m_window, text.c_str()); }

bool GlfwPlatform::updateGamepadMappings(const std::string& mappings) {
    return ensureGlfw() && glfwUpdateGamepadMappings(mappings.c_str()) == GLFW_TRUE;
}

} // namespace ox

#endif // OX_HAS_GLFW
