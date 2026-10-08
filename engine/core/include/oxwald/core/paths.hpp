#pragma once

#include <filesystem>
#include <string_view>

namespace ox::paths {

// Engine source tree (shaders, built-in assets). $OXWALD_SOURCE_DIR overrides the compiled-in
// OX_ENGINE_SOURCE_DIR so relocated/installed builds can point elsewhere.
[[nodiscard]] std::filesystem::path engineSourceDir();
// Absolute path of the running executable (empty on failure).
[[nodiscard]] std::filesystem::path executablePath();
[[nodiscard]] std::filesystem::path executableDir();
// Per-user writable data directory (not created):
//   macOS   ~/Library/Application Support/<app>
//   Linux   $XDG_DATA_HOME/<app> or ~/.local/share/<app>
//   Windows %APPDATA%\<app>
[[nodiscard]] std::filesystem::path userDataDir(std::string_view appName = "Oxwald");
[[nodiscard]] std::filesystem::path tempDir();

} // namespace ox::paths
