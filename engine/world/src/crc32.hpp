#pragma once

#include <oxwald/core/types.hpp>

#include <array>

namespace ox::world::detail {

inline constexpr std::array<u32, 256> kCrcTable = [] {
    std::array<u32, 256> t{};
    for (u32 i = 0; i < 256; ++i) {
        u32 c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        t[i] = c;
    }
    return t;
}();

// IEEE CRC-32 (PNG / zlib polynomial).
inline u32 crc32(const u8* data, usize n, u32 crc = 0) {
    crc = ~crc;
    for (usize i = 0; i < n; ++i) {
        crc = kCrcTable[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

} // namespace ox::world::detail
