#pragma once

// Fatal signal handler: SIGSEGV / SIGBUS / SIGFPE / SIGILL / SIGABRT / SIGSYS are logged to logcat (tag
// "OxwaldCrash": signal, code, fault address, thread, and an _Unwind_Backtrace with module + symbol per frame from
// dladdr), then the previous handler (Android's debuggerd) runs so a tombstone is still written:
//   adb shell ls /data/tombstones  (root/emulator)   |   adb bugreport   |   Android Studio "App Quality Insights"
// Symbolize addresses offline with ndk-stack:  adb logcat | ndk-stack -sym <build>/lib/<abi>
// A debugger (lldb) gets the signal before this handler, so breaking on crashes is unaffected.
namespace ox::android {

void installCrashHandler(const char* tag = "OxwaldCrash");

} // namespace ox::android
