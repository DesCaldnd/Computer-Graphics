#pragma once

#include <oxwald/core/hash.hpp>
#include <oxwald/core/types.hpp>

#include <compare>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace ox {

// 128-bit identifier (RFC 4122 version 4 when generated). Nil (all zero) means "none".
struct Uuid {
    u64 hi = 0;
    u64 lo = 0;

    [[nodiscard]] static Uuid generate();
    // Deterministic UUID from a name (FNV-based, version bits set to 8 = custom). Useful for tests/built-ins.
    [[nodiscard]] static Uuid fromName(std::string_view name);
    // Accepts canonical "8-4-4-4-12" form or 32 hex digits; returns nullopt on malformed input.
    [[nodiscard]] static std::optional<Uuid> parse(std::string_view text);

    [[nodiscard]] std::string toString() const;
    [[nodiscard]] constexpr bool isNil() const { return hi == 0 && lo == 0; }
    [[nodiscard]] constexpr bool isValid() const { return !isNil(); }
    explicit constexpr operator bool() const { return !isNil(); }
    [[nodiscard]] constexpr u64 hash() const { return hashCombine(hi, lo); }

    friend constexpr bool operator==(const Uuid&, const Uuid&) = default;
    friend constexpr std::strong_ordering operator<=>(const Uuid&, const Uuid&) = default;
};

} // namespace ox

template <>
struct std::hash<ox::Uuid> {
    std::size_t operator()(const ox::Uuid& id) const noexcept { return static_cast<std::size_t>(id.hash()); }
};
