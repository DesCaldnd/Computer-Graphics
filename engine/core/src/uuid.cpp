#include <oxwald/core/uuid.hpp>

#include <random>

namespace ox {
namespace {

std::mt19937_64& threadRng() {
    thread_local std::mt19937_64 rng = [] {
        std::random_device rd;
        std::seed_seq seq{rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd()};
        return std::mt19937_64(seq);
    }();
    return rng;
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

Uuid Uuid::generate() {
    auto& rng = threadRng();
    Uuid id{rng(), rng()};
    id.hi = (id.hi & ~0x000000000000F000ull) | 0x0000000000004000ull; // version 4
    id.lo = (id.lo & ~0xC000000000000000ull) | 0x8000000000000000ull; // variant 10xx
    if (id.isNil()) {
        id.lo = 1;
    }
    return id;
}

Uuid Uuid::fromName(std::string_view name) {
    Uuid id{mix64(fnv1a64(name)), mix64(fnv1a64(name, 0x84222325cbf29ce4ull))};
    id.hi = (id.hi & ~0x000000000000F000ull) | 0x0000000000008000ull;
    id.lo = (id.lo & ~0xC000000000000000ull) | 0x8000000000000000ull;
    return id;
}

std::optional<Uuid> Uuid::parse(std::string_view text) {
    Uuid id;
    int digits = 0;
    for (usize i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '-') {
            if (text.size() != 36 || (i != 8 && i != 13 && i != 18 && i != 23)) return std::nullopt;
            continue;
        }
        const int v = hexValue(c);
        if (v < 0 || digits >= 32) return std::nullopt;
        if (digits < 16) {
            id.hi = (id.hi << 4) | u64(v);
        } else {
            id.lo = (id.lo << 4) | u64(v);
        }
        ++digits;
    }
    if (digits != 32) return std::nullopt;
    return id;
}

std::string Uuid::toString() const {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string s(36, '-');
    int out = 0;
    for (int d = 0; d < 32; ++d) {
        if (out == 8 || out == 13 || out == 18 || out == 23) ++out;
        const u64 word = d < 16 ? hi : lo;
        const int shift = (15 - (d % 16)) * 4;
        s[usize(out++)] = kHex[(word >> shift) & 0xF];
    }
    return s;
}

} // namespace ox
