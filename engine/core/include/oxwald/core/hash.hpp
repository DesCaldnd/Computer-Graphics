#pragma once

#include <oxwald/core/types.hpp>

#include <functional>
#include <span>
#include <string_view>

namespace ox {

inline constexpr u64 kFnv64Offset = 0xcbf29ce484222325ull;
inline constexpr u64 kFnv64Prime = 0x100000001b3ull;

// FNV-1a 64. constexpr so string hashes can be compile-time constants / switch labels.
[[nodiscard]] constexpr u64 fnv1a64(std::string_view s, u64 seed = kFnv64Offset) {
    u64 h = seed;
    for (char c : s) {
        h ^= static_cast<u8>(c);
        h *= kFnv64Prime;
    }
    return h;
}

[[nodiscard]] inline u64 fnv1a64(std::span<const std::byte> bytes, u64 seed = kFnv64Offset) {
    u64 h = seed;
    for (std::byte b : bytes) {
        h ^= static_cast<u8>(b);
        h *= kFnv64Prime;
    }
    return h;
}

[[nodiscard]] constexpr u64 hashString(std::string_view s) { return fnv1a64(s); }

// Strong 64-bit mix (splitmix64 finaliser) for combining hashes.
[[nodiscard]] constexpr u64 mix64(u64 x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

[[nodiscard]] constexpr u64 hashCombine(u64 seed, u64 value) {
    return mix64(seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2)));
}

template <class T>
void hashCombineInto(u64& seed, const T& value) {
    seed = hashCombine(seed, static_cast<u64>(std::hash<T>{}(value)));
}

// Compile-time string hash literal: "r.Shadows"_hash
namespace literals {
consteval u64 operator""_hash(const char* s, usize n) { return fnv1a64(std::string_view(s, n)); }
} // namespace literals

// CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320) used for chunk integrity in archives.
[[nodiscard]] u32 crc32(std::span<const std::byte> data, u32 crc = 0);
[[nodiscard]] inline u32 crc32(const void* data, usize size, u32 crc = 0) {
    return crc32(std::span<const std::byte>(static_cast<const std::byte*>(data), size), crc);
}

} // namespace ox
