#pragma once

// Small helpers around the GameActivity Java object for android_main.
#include <oxwald/core/types.hpp>

#include <optional>
#include <string>

struct android_app;

namespace ox::android {

// Intent extras of the launching intent (adb shell am start -n <pkg>/<activity> --es args "..." --ez flag true).
[[nodiscard]] std::optional<std::string> intentStringExtra(android_app* app, const char* key);
[[nodiscard]] bool intentBoolExtra(android_app* app, const char* key, bool fallback = false);

// True while a tracer (lldb-server, gdbserver) is attached to this process (/proc/self/status TracerPid).
[[nodiscard]] bool debuggerAttached();
// Blocks until a native debugger attaches or the timeout expires (intent extra "wait_debugger"): the lldb scripts
// start the app, attach, set breakpoints and continue; the wait loop then ends by itself.
bool waitForDebugger(f64 timeoutSeconds);

// "arm64-v8a", "x86_64", ...
[[nodiscard]] const char* abiName();

} // namespace ox::android
