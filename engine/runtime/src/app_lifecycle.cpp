#include <oxwald/runtime/app_lifecycle.hpp>

#include <format>

namespace ox {

std::string_view appEventName(AppEvent e) {
    switch (e) {
    case AppEvent::Start: return "Start";
    case AppEvent::Resume: return "Resume";
    case AppEvent::Pause: return "Pause";
    case AppEvent::Stop: return "Stop";
    case AppEvent::WindowCreated: return "WindowCreated";
    case AppEvent::WindowDestroyed: return "WindowDestroyed";
    case AppEvent::WindowResized: return "WindowResized";
    case AppEvent::ConfigChanged: return "ConfigChanged";
    case AppEvent::FocusGained: return "FocusGained";
    case AppEvent::FocusLost: return "FocusLost";
    case AppEvent::LowMemory: return "LowMemory";
    case AppEvent::SaveState: return "SaveState";
    case AppEvent::Destroy: return "Destroy";
    }
    return "?";
}

std::string LifecycleActions::toString() const {
    std::string s;
    auto add = [&](bool on, const char* name) {
        if (!on) return;
        if (!s.empty()) s += ',';
        s += name;
    };
    add(createSurface, "createSurface");
    add(destroySurface, "destroySurface");
    add(recreateSwapchain, "recreateSwapchain");
    add(pauseAudio, "pauseAudio");
    add(resumeAudio, "resumeAudio");
    add(releaseInput, "releaseInput");
    add(saveState, "saveState");
    add(trimMemory, "trimMemory");
    add(quit, "quit");
    return s.empty() ? "none" : s;
}

LifecycleActions AppLifecycle::handle(AppEvent event) {
    LifecycleActions a;
    if (m_destroyed) return a; // nothing after Destroy
    const bool couldSimulate = shouldSimulate();
    switch (event) {
    case AppEvent::Start: m_started = true; break;
    case AppEvent::Resume:
        m_started = true;
        m_resumed = true;
        break;
    case AppEvent::Pause:
        if (m_resumed) a.saveState = true; // onPause is the last callback guaranteed before the process may die
        m_resumed = false;
        break;
    case AppEvent::Stop:
        if (m_started) a.saveState = true;
        m_started = false;
        m_resumed = false;
        break;
    case AppEvent::WindowCreated:
        // A second WindowCreated without WindowDestroyed means the window was replaced.
        if (m_window) a.destroySurface = true;
        m_window = true;
        a.createSurface = true;
        ++m_windowGeneration;
        break;
    case AppEvent::WindowDestroyed:
        if (m_window) {
            a.destroySurface = true;
            ++m_windowGeneration;
        }
        m_window = false;
        break;
    case AppEvent::WindowResized:
    case AppEvent::ConfigChanged: a.recreateSwapchain = m_window; break;
    case AppEvent::FocusGained: m_focused = true; break;
    case AppEvent::FocusLost:
        if (m_focused) a.releaseInput = true;
        m_focused = false;
        break;
    case AppEvent::LowMemory: a.trimMemory = true; break;
    case AppEvent::SaveState: a.saveState = true; break;
    case AppEvent::Destroy:
        a.destroySurface = m_window;
        a.saveState = m_started || m_resumed;
        a.quit = true;
        m_window = m_resumed = m_started = m_focused = false;
        m_destroyed = true;
        break;
    }
    const bool canSimulate = shouldSimulate();
    if (couldSimulate && !canSimulate) a.releaseInput = true;
    // Audio follows "the game is in front of the user": paused when we stop simulating, resumed when we start again.
    if (!canSimulate && !m_audioPaused && (couldSimulate || event == AppEvent::Pause || event == AppEvent::Stop ||
                                           event == AppEvent::Destroy)) {
        a.pauseAudio = true;
        m_audioPaused = true;
    }
    if (canSimulate && m_audioPaused) {
        a.resumeAudio = true;
        m_audioPaused = false;
    }
    return a;
}

std::string AppLifecycle::describe() const {
    return std::format("started={} resumed={} window={} focused={} destroyed={} gen={}", m_started, m_resumed, m_window,
                       m_focused, m_destroyed, m_windowGeneration);
}

} // namespace ox
