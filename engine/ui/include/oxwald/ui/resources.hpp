#pragma once

// Location of the ui module's shipped resources (fonts, sample .rml/.rcss):
//   $OXWALD_UI_RESOURCE_DIR, else <exe dir>/ui (packaged builds), else engine/ui/resources (dev builds).

#include <oxwald/core/types.hpp>

#include <filesystem>
#include <string_view>
#include <vector>

namespace ox::ui {

[[nodiscard]] std::filesystem::path resourceDir();
// Reads resourceDir()/relative; empty on failure (logged).
[[nodiscard]] std::vector<u8> readResource(std::string_view relative);

inline constexpr std::string_view kFontUi = "fonts/Inter-Regular.ttf";        // Latin + Cyrillic + Greek
inline constexpr std::string_view kFontUiBold = "fonts/Inter-Bold.ttf";
inline constexpr std::string_view kFontMono = "fonts/JetBrainsMono-Regular.ttf"; // Latin + Cyrillic

} // namespace ox::ui
