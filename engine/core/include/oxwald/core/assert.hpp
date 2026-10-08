#pragma once

#include <oxwald/core/log.hpp>

namespace ox::detail {
[[noreturn]] void assertFailed(const char* expr, const char* file, int line, const std::string& message);
} // namespace ox::detail

// OX_ASSERT is active in all builds: engine invariants are cheap to check and
// silent corruption in a renderer is far more expensive to debug.
#define OX_ASSERT(expr, ...)                                                                        \
    do {                                                                                           \
        if (!(expr)) [[unlikely]] {                                                                \
            ::ox::detail::assertFailed(#expr, __FILE__, __LINE__, ::std::string{__VA_OPT__(::std::format(__VA_ARGS__))}); \
        }                                                                                          \
    } while (false)

#define OX_UNREACHABLE() ::ox::detail::assertFailed("unreachable", __FILE__, __LINE__, {})
