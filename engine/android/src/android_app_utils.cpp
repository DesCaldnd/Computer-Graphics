#include <oxwald/android/android_app_utils.hpp>
#include <oxwald/core/log.hpp>

#include <game-activity/GameActivity.h>
#include <game-activity/native_app_glue/android_native_app_glue.h>
#include <jni.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace ox::android {

namespace {

// Attaches the calling thread for the duration of one call (android_main runs on a native thread).
class JniScope {
public:
    explicit JniScope(android_app* app) : m_vm(app->activity->vm) {
        if (m_vm->GetEnv(reinterpret_cast<void**>(&m_env), JNI_VERSION_1_6) == JNI_EDETACHED) {
            if (m_vm->AttachCurrentThread(&m_env, nullptr) == JNI_OK) m_attached = true;
            else m_env = nullptr;
        }
    }
    ~JniScope() {
        if (m_attached) m_vm->DetachCurrentThread();
    }
    JNIEnv* env() const { return m_env; }

private:
    JavaVM* m_vm;
    JNIEnv* m_env = nullptr;
    bool m_attached = false;
};

jobject currentIntent(JNIEnv* env, jobject activity) {
    jclass cls = env->GetObjectClass(activity);
    jmethodID getIntent = env->GetMethodID(cls, "getIntent", "()Landroid/content/Intent;");
    env->DeleteLocalRef(cls);
    return getIntent ? env->CallObjectMethod(activity, getIntent) : nullptr;
}

void clearException(JNIEnv* env) {
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
}

} // namespace

std::optional<std::string> intentStringExtra(android_app* app, const char* key) {
    JniScope jni(app);
    JNIEnv* env = jni.env();
    if (!env) return std::nullopt;
    jobject intent = currentIntent(env, app->activity->javaGameActivity);
    if (!intent) {
        clearException(env);
        return std::nullopt;
    }
    jclass icls = env->GetObjectClass(intent);
    jmethodID getStringExtra = env->GetMethodID(icls, "getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;");
    jstring jkey = env->NewStringUTF(key);
    auto value = static_cast<jstring>(env->CallObjectMethod(intent, getStringExtra, jkey));
    clearException(env);
    std::optional<std::string> out;
    if (value) {
        const char* chars = env->GetStringUTFChars(value, nullptr);
        out = std::string(chars ? chars : "");
        env->ReleaseStringUTFChars(value, chars);
        env->DeleteLocalRef(value);
    }
    env->DeleteLocalRef(jkey);
    env->DeleteLocalRef(icls);
    env->DeleteLocalRef(intent);
    return out;
}

bool intentBoolExtra(android_app* app, const char* key, bool fallback) {
    JniScope jni(app);
    JNIEnv* env = jni.env();
    if (!env) return fallback;
    jobject intent = currentIntent(env, app->activity->javaGameActivity);
    if (!intent) {
        clearException(env);
        return fallback;
    }
    jclass icls = env->GetObjectClass(intent);
    jmethodID getBool = env->GetMethodID(icls, "getBooleanExtra", "(Ljava/lang/String;Z)Z");
    jstring jkey = env->NewStringUTF(key);
    const bool v = env->CallBooleanMethod(intent, getBool, jkey, jboolean(fallback)) != JNI_FALSE;
    clearException(env);
    env->DeleteLocalRef(jkey);
    env->DeleteLocalRef(icls);
    env->DeleteLocalRef(intent);
    // `--es key true` (string) is accepted as well.
    if (v == fallback) {
        if (auto s = intentStringExtra(app, key)) return *s == "1" || *s == "true" || *s == "on";
    }
    return v;
}

bool debuggerAttached() {
    FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return false;
    char line[256];
    bool attached = false;
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "TracerPid:", 10) == 0) {
            attached = std::atoi(line + 10) != 0;
            break;
        }
    }
    std::fclose(f);
    return attached;
}

bool waitForDebugger(f64 timeoutSeconds) {
    OX_LOG_INFO("android", "waiting up to {:.0f} s for a native debugger (pid {}) - tools/android/debug.ps1 attaches lldb",
                timeoutSeconds, getpid());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<f64>(timeoutSeconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (debuggerAttached()) {
            OX_LOG_INFO("android", "debugger attached");
            // Give lldb a moment to finish loading modules and resolving breakpoints before running on.
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    OX_LOG_WARN("android", "no debugger attached, continuing");
    return false;
}

const char* abiName() {
#if defined(__aarch64__)
    return "arm64-v8a";
#elif defined(__x86_64__)
    return "x86_64";
#elif defined(__arm__)
    return "armeabi-v7a";
#elif defined(__i386__)
    return "x86";
#else
    return "unknown";
#endif
}

} // namespace ox::android
