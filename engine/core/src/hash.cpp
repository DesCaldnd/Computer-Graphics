#include <oxwald/core/hash.hpp>

#include <array>

namespace ox {
namespace {

constexpr std::array<u32, 256> makeCrcTable() {
    std::array<u32, 256> table{};
    for (u32 i = 0; i < 256; ++i) {
        u32 c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
    return table;
}

constexpr auto kCrcTable = makeCrcTable();

} // namespace

u32 crc32(std::span<const std::byte> data, u32 crc) {
    crc = ~crc;
    for (std::byte b : data) {
        crc = kCrcTable[(crc ^ static_cast<u8>(b)) & 0xFFu] ^ (crc >> 8);
    }
    return ~crc;
}

} // namespace ox
