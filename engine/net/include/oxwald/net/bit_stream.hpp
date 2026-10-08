#pragma once

#include <oxwald/core/types.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ox::net {

// Number of bits needed to store any value in [0, range].
constexpr u32 bitsRequired(u64 range) {
    u32 bits = 0;
    while (range != 0) {
        ++bits;
        range >>= 1;
    }
    return bits;
}

// Quantisation helpers shared by the writer, the reader and the property codecs (so "equal after quantisation"
// can be decided without serialising).
u32 quantizedFloatSteps(f32 min, f32 max, f32 resolution);
u32 quantizeFloat(f32 value, f32 min, f32 max, u32 steps);
f32 dequantizeFloat(u32 index, f32 min, f32 max, u32 steps);

// LSB-first bit packer. Writes never fail; the buffer grows as needed.
class BitWriter {
public:
    BitWriter() = default;

    void writeBits(u64 value, u32 bits);
    void writeBool(bool value) { writeBits(value ? 1 : 0, 1); }
    void writeU8(u8 value) { writeBits(value, 8); }
    void writeU16(u16 value) { writeBits(value, 16); }
    void writeU32(u32 value) { writeBits(value, 32); }
    void writeU64(u64 value) { writeBits(value, 64); }

    // LEB128-style varints, signed values are zig-zag encoded.
    void writeVarU32(u32 value) { writeVarU64(value); }
    void writeVarU64(u64 value);
    void writeVarI32(i32 value) { writeVarI64(value); }
    void writeVarI64(i64 value);

    // Exactly bitsRequired(max - min) bits; value is clamped into the range.
    void writeRangedInt(i64 value, i64 min, i64 max);

    void writeF32(f32 value);
    void writeF64(f64 value);
    // Uniform quantisation: |decoded - value| <= resolution / 2 for values inside [min, max].
    void writeQuantizedFloat(f32 value, f32 min, f32 max, f32 resolution);
    void writeQuantizedVec3(const glm::vec3& value, f32 min, f32 max, f32 resolution);
    void writeVec2(const glm::vec2& value);
    void writeVec3(const glm::vec3& value);

    // "Smallest three": 2 bits for the index of the largest component + 3 components in [-1/sqrt2, 1/sqrt2].
    void writeQuat(const glm::quat& value, u32 bitsPerComponent = 10);
    void writeQuatRaw(const glm::quat& value);

    void writeString(std::string_view value);
    void writeBytes(std::span<const u8> bytes);
    void alignToByte();

    // Rollback support used for budgeted snapshot writing.
    u32 bitPosition() const { return m_bits; }
    void rewind(u32 bitPosition);

    u32 bitsWritten() const { return m_bits; }
    usize bytesWritten() const { return (m_bits + 7) / 8; }
    std::span<const u8> data() const { return {m_buffer.data(), bytesWritten()}; }
    std::vector<u8> takeData();
    void clear();

private:
    std::vector<u8> m_buffer;
    u32 m_bits = 0;
};

// Reader counterpart. Reading past the end (or malformed varints) sets an error flag and yields zeros;
// callers check ok() once after a batch of reads instead of after each value.
class BitReader {
public:
    BitReader() = default;
    explicit BitReader(std::span<const u8> data) : m_data(data) {}

    u64 readBits(u32 bits);
    bool readBool() { return readBits(1) != 0; }
    u8 readU8() { return static_cast<u8>(readBits(8)); }
    u16 readU16() { return static_cast<u16>(readBits(16)); }
    u32 readU32() { return static_cast<u32>(readBits(32)); }
    u64 readU64() { return readBits(64); }

    u32 readVarU32();
    u64 readVarU64();
    i32 readVarI32() { return static_cast<i32>(readVarI64()); }
    i64 readVarI64();

    i64 readRangedInt(i64 min, i64 max);

    f32 readF32();
    f64 readF64();
    f32 readQuantizedFloat(f32 min, f32 max, f32 resolution);
    glm::vec3 readQuantizedVec3(f32 min, f32 max, f32 resolution);
    glm::vec2 readVec2();
    glm::vec3 readVec3();
    glm::quat readQuat(u32 bitsPerComponent = 10);
    glm::quat readQuatRaw();

    // maxLength guards against hostile length prefixes.
    std::string readString(u32 maxLength = 1u << 20);
    void readBytes(std::span<u8> out);
    void alignToByte();

    bool ok() const { return !m_error; }
    void setError() { m_error = true; }
    u32 bitPosition() const { return m_bit; }
    u32 bitsRemaining() const;
    bool atEnd() const { return bitsRemaining() < 8; }

private:
    std::span<const u8> m_data;
    u32 m_bit = 0;
    bool m_error = false;
};

} // namespace ox::net
