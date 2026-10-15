#pragma once

// Mobile application lifecycle as a pure state machine (no OS calls), driven by the platform layer (Android
// GameActivity commands; also usable for iOS-style lifecycles) and unit tested on the host.
//
// Android delivers, roughly: Start -> Resume -> WindowCreated -> FocusGained ... FocusLost -> Pause ->
// WindowDestroyed -> Stop (-> SaveState) ... and back, or Destroy. Orders vary between devices and versions
// (window destroyed before/after pause, focus before/after resume, rotation = destroy + create or only resize),
// so every event only updates its own flag and the derived predicates decide what runs:
//
//   shouldRender()    resumed (or at least started) and a native window exists -> render and present
//   shouldSimulate()  resumed and has a window and focus -> advance game time
//
// handle() returns what the platform has to do for that transition (create/destroy the Vulkan surface,
// pause/resume audio, release held input, persist state, ...). Repeated events return no actions.
#include <oxwald/core/types.hpp>

#include <string>
#include <string_view>

namespace ox {

enum class AppEvent : u8 {
    Start,
    Resume,
    Pause,
    Stop,
    WindowCreated,
    WindowDestroyed,
    WindowResized,      // size or content rect changed
    ConfigChanged,      // rotation, density, locale ...
    FocusGained,
    FocusLost,
    LowMemory,
    SaveState,
    Destroy,
};

[[nodiscard]] std::string_view appEventName(AppEvent e);

struct LifecycleActions {
    bool createSurface = false;     // a new native window: (re)create the Vulkan surface + swapchain
    bool destroySurface = false;    // the window is going away: release swapchain + surface now
    bool recreateSwapchain = false; // same window, new size / transform
    bool pauseAudio = false;
    bool resumeAudio = false;
    bool releaseInput = false;      // send key/button releases (fingers lifted while we were not looking)
    bool saveState = false;         // persist settings / quick save: the process may be killed after this
    bool trimMemory = false;        // drop caches (streaming pools, pipeline cache flush)
    bool quit = false;              // leave the game loop

    [[nodiscard]] bool any() const {
        return createSurface || destroySurface || recreateSwapchain || pauseAudio || resumeAudio || releaseInput ||
               saveState || trimMemory || quit;
    }
    [[nodiscard]] std::string toString() const;
};

class AppLifecycle {
public:
    LifecycleActions handle(AppEvent event);

    [[nodiscard]] bool started() const { return m_started; }
    [[nodiscard]] bool resumed() const { return m_resumed; }
    [[nodiscard]] bool hasWindow() const { return m_window; }
    [[nodiscard]] bool focused() const { return m_focused; }
    [[nodiscard]] bool destroyed() const { return m_destroyed; }

    [[nodiscard]] bool shouldRender() const { return !m_destroyed && m_window && (m_resumed || m_started); }
    [[nodiscard]] bool shouldSimulate() const { return !m_destroyed && m_window && m_resumed && m_focused; }
    // Incremented on every WindowCreated / WindowDestroyed (matches ISurfaceProvider::surfaceGeneration use).
    [[nodiscard]] u64 windowGeneration() const { return m_windowGeneration; }
    [[nodiscard]] std::string describe() const;

private:
    bool m_started = false;
    bool m_resumed = false;
    bool m_window = false;
    bool m_focused = false;
    bool m_destroyed = false;
    bool m_audioPaused = false;
    u64 m_windowGeneration = 0;
};

} // namespace ox
