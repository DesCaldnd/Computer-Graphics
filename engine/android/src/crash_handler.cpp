#include <oxwald/android/crash_handler.hpp>

#include <android/log.h>
#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>
#include <unwind.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ox::android {

namespace {

constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT, SIGSYS};
constexpr int kMaxSignal = 64;
struct sigaction g_previous[kMaxSignal];
const char* g_tag = "OxwaldCrash";
volatile sig_atomic_t g_inHandler = 0;

struct BacktraceState {
    uintptr_t* frames;
    int count;
    int max;
};

_Unwind_Reason_Code unwindCallback(_Unwind_Context* context, void* arg) {
    auto* state = static_cast<BacktraceState*>(arg);
    const uintptr_t pc = _Unwind_GetIP(context);
    if (pc) {
        if (state->count >= state->max) return _URC_END_OF_STACK;
        state->frames[state->count++] = pc;
    }
    return _URC_NO_REASON;
}

const char* signalName(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGFPE: return "SIGFPE";
    case SIGILL: return "SIGILL";
    case SIGABRT: return "SIGABRT";
    case SIGSYS: return "SIGSYS";
    default: return "signal";
    }
}

void handler(int sig, siginfo_t* info, void* ucontext) {
    if (!g_inHandler) {
        g_inHandler = 1;
        char threadName[32] = "?";
        if (FILE* f = std::fopen("/proc/self/comm", "r")) { // best effort, the process is going down anyway
            if (std::fgets(threadName, sizeof(threadName), f)) threadName[std::strcspn(threadName, "\n")] = 0;
            std::fclose(f);
        }
        __android_log_print(ANDROID_LOG_FATAL, g_tag, "*** %s (signal %d, code %d) fault address %p, pid %d tid %d (%s)",
                            signalName(sig), sig, info ? info->si_code : 0, info ? info->si_addr : nullptr, getpid(),
                            gettid(), threadName);
        uintptr_t frames[64];
        BacktraceState state{frames, 0, 64};
        _Unwind_Backtrace(unwindCallback, &state);
        for (int i = 0; i < state.count; ++i) {
            Dl_info dl{};
            const void* pc = reinterpret_cast<const void*>(frames[i]);
            if (dladdr(pc, &dl) && dl.dli_fname) {
                const uintptr_t rel = frames[i] - reinterpret_cast<uintptr_t>(dl.dli_fbase);
                const char* lib = std::strrchr(dl.dli_fname, '/');
                __android_log_print(ANDROID_LOG_FATAL, g_tag, "  #%02d pc %08zx  %s (%s+%zu)", i, std::size_t(rel),
                                    lib ? lib + 1 : dl.dli_fname, dl.dli_sname ? dl.dli_sname : "???",
                                    std::size_t(dl.dli_saddr ? frames[i] - reinterpret_cast<uintptr_t>(dl.dli_saddr) : 0));
            } else {
                __android_log_print(ANDROID_LOG_FATAL, g_tag, "  #%02d pc %p", i, pc);
            }
        }
        __android_log_print(ANDROID_LOG_FATAL, g_tag,
                            "*** symbolize: adb logcat | ndk-stack -sym <build>/lib/<abi>; tombstone follows (debuggerd)");
    }
    // Hand over to the previous handler (debuggerd writes the tombstone) or the default action.
    if (sig > 0 && sig < kMaxSignal) {
        const struct sigaction& prev = g_previous[sig];
        if ((prev.sa_flags & SA_SIGINFO) && prev.sa_sigaction) {
            prev.sa_sigaction(sig, info, ucontext);
            return;
        }
        if (prev.sa_handler != SIG_DFL && prev.sa_handler != SIG_IGN && prev.sa_handler) {
            prev.sa_handler(sig);
            return;
        }
        signal(sig, SIG_DFL);
        // A kernel generated fault re-executes the faulting instruction on return and dies with the default
        // action; signals sent by kill/abort are raised again.
        if (!info || info->si_code <= 0) raise(sig);
    }
}

} // namespace

void installCrashHandler(const char* tag) {
    static bool installed = false;
    if (installed) return;
    installed = true;
    g_tag = tag;
    // Alternate stack: stack overflows must still be able to log.
    static char altStack[64 * 1024];
    stack_t ss{};
    ss.ss_sp = altStack;
    ss.ss_size = sizeof(altStack);
    sigaltstack(&ss, nullptr);
    for (int sig : kSignals) {
        struct sigaction sa {};
        sa.sa_sigaction = handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigaction(sig, &sa, &g_previous[sig]);
    }
}

} // namespace ox::android
