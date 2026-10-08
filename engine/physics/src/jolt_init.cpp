#include "jolt_common.hpp"

#include <Jolt/ConfigurationString.h>

#include <oxwald/core/assert.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/physics/physics.hpp>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace ox::physics {
namespace {

void joltTrace(const char* fmt, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    OX_LOG_INFO("physics", "Jolt: {}", buffer);
}

#ifdef JPH_ENABLE_ASSERTS
bool joltAssertFailed(const char* expression, const char* message, const char* file, JPH::uint line) {
    OX_LOG_ERROR("physics", "Jolt assert {}:{}: ({}) {}", file, line, expression, message ? message : "");
    return true; // break into the debugger
}
#endif

std::string makeBackendInfo() {
    return std::format("Jolt {}.{}.{}: {}", JPH_VERSION_MAJOR, JPH_VERSION_MINOR, JPH_VERSION_PATCH,
                       JPH::GetConfigurationString());
}

} // namespace

void detail::ensureJoltInitialized() {
    static std::once_flag once;
    std::call_once(once, [] {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = &joltTrace;
        JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = &joltAssertFailed;)
        // A mismatch means the engine was compiled with different JPH_* feature defines than the vcpkg
        // library (double precision, debug renderer, profiler, asserts, object stream...) → memory
        // layout differences and crashes. Fail loudly instead.
        OX_ASSERT(JPH::VerifyJoltVersionID(), "Jolt build configuration mismatch (JPH_VERSION_ID): {}",
                  makeBackendInfo());
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
        OX_LOG_INFO("physics", "Initialized {}", makeBackendInfo());
    });
}

const char* backendInfo() {
    static const std::string info = makeBackendInfo();
    return info.c_str();
}

} // namespace ox::physics
