#pragma once

// logcat integration.
//   installLogcatSink()      ox::log records -> __android_log_write (tag "Oxwald", priority from the level,
//                            message "[category] text"); the stderr sink is switched off (stderr goes nowhere on
//                            Android unless redirected).
//   redirectStdioToLogcat()  stdout/stderr (printf, std::cout, third-party prints) -> logcat line by line through a
//                            pipe and a reader thread (tag "OxwaldStdout", INFO / WARN).
// Filter with:  adb logcat -s Oxwald OxwaldStdout OxwaldCrash vulkan   (tools/android/logcat.ps1)
namespace ox::android {

inline constexpr const char* kLogTag = "Oxwald";
inline constexpr const char* kStdoutTag = "OxwaldStdout";

void installLogcatSink(const char* tag = kLogTag);
bool redirectStdioToLogcat(const char* tag = kStdoutTag);

} // namespace ox::android
