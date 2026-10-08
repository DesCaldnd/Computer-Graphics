#include <oxwald/net/bit_stream.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <utility>

namespace ox::net {

namespace {
constexpr f32 kQuatComponentMax = 0.70710678118654752f; // 1/sqrt(2)

u64 zigzag(i64 v) { return (static_cast<u64>(v) << 1) ^ static_cast<u64>(v >> 63); }
i64 unzigzag(u64 v) { return static_cast<i64>(v >> 1) ^ -static_cast<i64>(v & 1); }
} // namespace

u32 quantizedFloatSteps(f32 min, f32 max, f32 resolution) {
    const f64 steps = std::ceil(static_cast<f64>(max - min) / static_cast<f64>(resolution));
    return static_cast<u32>(std::clamp(steps, 1.0, 4294967295.0));
}

u32 quantizeFloat(f32 value, f32 min, f32 max, u32 steps) {
    const f64 t = (static_cast<f64>(std::clamp(value, min, max)) - min) / (static_cast<f64>(max) - min);
    return static_cast<u32>(std::llround(t * steps));
}

f32 dequantizeFloat(u32 index, f32 min, f32 max, u32 steps) {
    return static_cast<f32>(min + (static_cast<f64>(max) - min) * (static_cast<f64>(index) / steps));
}

// ---------------------------------------------------------------- BitWriter

void BitWriter::writeBits(u64 value, u32 bits) {
    if (bits == 0) {
        return;
    }
    if (bits < 64) {
        value &= (u64{1} << bits) - 1;
    }
    const usize needed = (m_bits + bits + 7) / 8;
    if (m_buffer.size() < needed) {
        m_buffer.resize(std::max(needed, m_buffer.size() * 2), 0);
    }
    while (bits > 0) {
        const u32 byteIndex = m_bits >> 3;
        const u32 offset = m_bits & 7;
        const u32 n = std::min(8 - offset, bits);
        m_buffer[byteIndex] |= static_cast<u8>((value & ((1u << n) - 1)) << offset);
        value >>= n;
        bits -= n;
        m_bits += n;
    }
}

void BitWriter::writeVarU64(u64 value) {
    do {
        u8 byte = value & 0x7F;
        value >>= 7;
        if (value != 0) {
            byte |= 0x80;
        }
        writeBits(byte, 8);
    } while (value != 0);
}

void BitWriter::writeVarI64(i64 value) { writeVarU64(zigzag(value)); }

void BitWriter::writeRangedInt(i64 value, i64 min, i64 max) {
    value = std::clamp(value, min, max);
    writeBits(static_cast<u64>(value - min), bitsRequired(static_cast<u64>(max - min)));
}

void BitWriter::writeF32(f32 value) { writeBits(std::bit_cast<u32>(value), 32); }
void BitWriter::writeF64(f64 value) { writeBits(std::bit_cast<u64>(value), 64); }

void BitWriter::writeQuantizedFloat(f32 value, f32 min, f32 max, f32 resolution) {
    const u32 steps = quantizedFloatSteps(min, max, resolution);
    writeBits(quantizeFloat(value, min, max, steps), bitsRequired(steps));
}

void BitWriter::writeQuantizedVec3(const glm::vec3& value, f32 min, f32 max, f32 resolution) {
    for (int i = 0; i < 3; ++i) {
        writeQuantizedFloat(value[i], min, max, resolution);
    }
}

void BitWriter::writeVec2(const glm::vec2& value) {
    writeF32(value.x);
    writeF32(value.y);
}

void BitWriter::writeVec3(const glm::vec3& value) {
    writeF32(value.x);
    writeF32(value.y);
    writeF32(value.z);
}

void BitWriter::writeQuat(const glm::quat& value, u32 bitsPerComponent) {
    const glm::quat q = glm::normalize(value);
    f32 c[4] = {q.x, q.y, q.z, q.w};
    u32 largest = 0;
    for (u32 i = 1; i < 4; ++i) {
        if (std::abs(c[i]) > std::abs(c[largest])) {
            largest = i;
        }
    }
    // q and -q are the same rotation: make the dropped component positive so it can be rebuilt with sqrt.
    const f32 sign = c[largest] < 0.f ? -1.f : 1.f;
    const u32 steps = (1u << bitsPerComponent) - 1;
    writeBits(largest, 2);
    for (u32 i = 0; i < 4; ++i) {
        if (i != largest) {
            writeBits(quantizeFloat(c[i] * sign, -kQuatComponentMax, kQuatComponentMax, steps), bitsPerComponent);
        }
    }
}

void BitWriter::writeQuatRaw(const glm::quat& value) {
    writeF32(value.x);
    writeF32(value.y);
    writeF32(value.z);
    writeF32(value.w);
}

void BitWriter::writeString(std::string_view value) {
    writeVarU32(static_cast<u32>(value.size()));
    writeBytes({reinterpret_cast<const u8*>(value.data()), value.size()});
}

void BitWriter::writeBytes(std::span<const u8> bytes) {
    if ((m_bits & 7) == 0) {
        const usize start = m_bits / 8;
        if (m_buffer.size() < start + bytes.size()) {
            m_buffer.resize(std::max(start + bytes.size(), m_buffer.size() * 2), 0);
        }
        if (!bytes.empty()) {
            std::memcpy(m_buffer.data() + start, bytes.data(), bytes.size());
        }
        m_bits += static_cast<u32>(bytes.size() * 8);
        return;
    }
    for (u8 b : bytes) {
        writeBits(b, 8);
    }
}

void BitWriter::alignToByte() {
    const u32 pad = (8 - (m_bits & 7)) & 7;
    writeBits(0, pad);
}

void BitWriter::rewind(u32 bitPosition) {
    if (bitPosition >= m_bits) {
        return;
    }
    const usize keepBytes = (bitPosition + 7) / 8;
    std::fill(m_buffer.begin() + static_cast<std::ptrdiff_t>(keepBytes), m_buffer.end(), u8{0});
    if ((bitPosition & 7) != 0) {
        m_buffer[bitPosition / 8] &= static_cast<u8>((1u << (bitPosition & 7)) - 1);
    }
    m_bits = bitPosition;
}

std::vector<u8> BitWriter::takeData() {
    m_buffer.resize(bytesWritten());
    std::vector<u8> out = std::move(m_buffer);
    clear();
    return out;
}

void BitWriter::clear() {
    m_buffer.clear();
    m_bits = 0;
}

// ---------------------------------------------------------------- BitReader

u32 BitReader::bitsRemaining() const {
    const u64 total = static_cast<u64>(m_data.size()) * 8;
    return m_bit >= total ? 0 : static_cast<u32>(total - m_bit);
}

u64 BitReader::readBits(u32 bits) {
    if (bits == 0) {
        return 0;
    }
    if (m_error || bits > 64 || bitsRemaining() < bits) {
        m_error = true;
        return 0;
    }
    u64 value = 0;
    u32 shift = 0;
    while (bits > 0) {
        const u32 byteIndex = m_bit >> 3;
        const u32 offset = m_bit & 7;
        const u32 n = std::min(8 - offset, bits);
        const u64 chunk = (m_data[byteIndex] >> offset) & ((1u << n) - 1);
        value |= chunk << shift;
        shift += n;
        bits -= n;
        m_bit += n;
    }
    return value;
}

u64 BitReader::readVarU64() {
    u64 value = 0;
    for (u32 shift = 0; shift < 70; shift += 7) {
        const u64 byte = readBits(8);
        if (m_error) {
            return 0;
        }
        value |= (byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            return value;
        }
    }
    m_error = true;
    return 0;
}

u32 BitReader::readVarU32() {
    const u64 v = readVarU64();
    if (v > 0xFFFFFFFFull) {
        m_error = true;
        return 0;
    }
    return static_cast<u32>(v);
}

i64 BitReader::readVarI64() { return unzigzag(readVarU64()); }

i64 BitReader::readRangedInt(i64 min, i64 max) {
    const u64 raw = readBits(bitsRequired(static_cast<u64>(max - min)));
    const i64 value = min + static_cast<i64>(raw);
    if (value > max) {
        m_error = true;
        return min;
    }
    return value;
}

f32 BitReader::readF32() { return std::bit_cast<f32>(static_cast<u32>(readBits(32))); }
f64 BitReader::readF64() { return std::bit_cast<f64>(readBits(64)); }

f32 BitReader::readQuantizedFloat(f32 min, f32 max, f32 resolution) {
    const u32 steps = quantizedFloatSteps(min, max, resolution);
    const u32 index = static_cast<u32>(readBits(bitsRequired(steps)));
    return dequantizeFloat(std::min(index, steps), min, max, steps);
}

glm::vec3 BitReader::readQuantizedVec3(f32 min, f32 max, f32 resolution) {
    glm::vec3 v;
    for (int i = 0; i < 3; ++i) {
        v[i] = readQuantizedFloat(min, max, resolution);
    }
    return v;
}

glm::vec2 BitReader::readVec2() {
    glm::vec2 v;
    v.x = readF32();
    v.y = readF32();
    return v;
}

glm::vec3 BitReader::readVec3() {
    glm::vec3 v;
    v.x = readF32();
    v.y = readF32();
    v.z = readF32();
    return v;
}

glm::quat BitReader::readQuat(u32 bitsPerComponent) {
    const u32 largest = static_cast<u32>(readBits(2));
    const u32 steps = (1u << bitsPerComponent) - 1;
    f32 c[4] = {};
    f32 sumSq = 0.f;
    for (u32 i = 0; i < 4; ++i) {
        if (i != largest) {
            c[i] = dequantizeFloat(static_cast<u32>(readBits(bitsPerComponent)), -kQuatComponentMax, kQuatComponentMax,
                                   steps);
            sumSq += c[i] * c[i];
        }
    }
    c[largest] = std::sqrt(std::max(0.f, 1.f - sumSq));
    return glm::normalize(glm::quat(c[3], c[0], c[1], c[2]));
}

glm::quat BitReader::readQuatRaw() {
    const f32 x = readF32();
    const f32 y = readF32();
    const f32 z = readF32();
    const f32 w = readF32();
    return glm::quat(w, x, y, z);
}

std::string BitReader::readString(u32 maxLength) {
    const u32 len = readVarU32();
    if (m_error || len > maxLength || len * 8ull > bitsRemaining()) {
        m_error = true;
        return {};
    }
    std::string s(len, '\0');
    readBytes({reinterpret_cast<u8*>(s.data()), s.size()});
    return s;
}

void BitReader::readBytes(std::span<u8> out) {
    if (bitsRemaining() < out.size() * 8) {
        m_error = true;
        std::fill(out.begin(), out.end(), u8{0});
        return;
    }
    if ((m_bit & 7) == 0) {
        std::memcpy(out.data(), m_data.data() + m_bit / 8, out.size());
        m_bit += static_cast<u32>(out.size() * 8);
        return;
    }
    for (u8& b : out) {
        b = static_cast<u8>(readBits(8));
    }
}

void BitReader::alignToByte() {
    const u32 pad = (8 - (m_bit & 7)) & 7;
    readBits(pad);
}

} // namespace ox::net
