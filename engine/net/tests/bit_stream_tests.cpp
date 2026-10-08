#include <oxwald/net/bit_stream.hpp>
#include <oxwald/net/serialize.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <glm/gtc/quaternion.hpp>
#include <random>

using namespace ox;
using namespace ox::net;

TEST(BitStream, RoundTripMixedValues) {
    BitWriter w;
    w.writeBits(5, 3);
    w.writeBool(true);
    w.writeBits(0xABCDEF0123456789ull, 64);
    w.writeVarU32(0);
    w.writeVarU32(127);
    w.writeVarU32(128);
    w.writeVarU64(~0ull);
    w.writeVarI32(-1);
    w.writeVarI64(std::numeric_limits<i64>::min());
    w.writeRangedInt(-3, -10, 10);
    w.writeF32(3.25f);
    w.writeF64(-1e300);
    w.writeString("hello, мир");
    w.writeVec3({1.f, -2.f, 3.5f});
    w.writeU16(0xBEEF);

    BitReader r(w.data());
    EXPECT_EQ(r.readBits(3), 5u);
    EXPECT_TRUE(r.readBool());
    EXPECT_EQ(r.readBits(64), 0xABCDEF0123456789ull);
    EXPECT_EQ(r.readVarU32(), 0u);
    EXPECT_EQ(r.readVarU32(), 127u);
    EXPECT_EQ(r.readVarU32(), 128u);
    EXPECT_EQ(r.readVarU64(), ~0ull);
    EXPECT_EQ(r.readVarI32(), -1);
    EXPECT_EQ(r.readVarI64(), std::numeric_limits<i64>::min());
    EXPECT_EQ(r.readRangedInt(-10, 10), -3);
    EXPECT_EQ(r.readF32(), 3.25f);
    EXPECT_EQ(r.readF64(), -1e300);
    EXPECT_EQ(r.readString(), "hello, мир");
    EXPECT_EQ(r.readVec3(), glm::vec3(1.f, -2.f, 3.5f));
    EXPECT_EQ(r.readU16(), 0xBEEF);
    EXPECT_TRUE(r.ok());
}

TEST(BitStream, VarintSizes) {
    BitWriter w;
    w.writeVarU32(127);
    EXPECT_EQ(w.bytesWritten(), 1u);
    w.writeVarU32(16383);
    EXPECT_EQ(w.bytesWritten(), 3u);
    w.writeVarI32(-64); // zig-zag keeps small negatives small
    EXPECT_EQ(w.bytesWritten(), 4u);
}

TEST(BitStream, RangedIntUsesMinimalBits) {
    BitWriter w;
    w.writeRangedInt(7, 0, 7);
    EXPECT_EQ(w.bitsWritten(), 3u);
    w.writeRangedInt(100, 100, 100); // single-value range costs nothing
    EXPECT_EQ(w.bitsWritten(), 3u);
    w.writeRangedInt(1000, -1000, 1000);
    EXPECT_EQ(w.bitsWritten(), 3u + 11u);
}

TEST(BitStream, OverflowSetsErrorInsteadOfCrashing) {
    BitWriter w;
    w.writeU8(200);
    BitReader r(w.data());
    EXPECT_EQ(r.readU8(), 200);
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.readU32(), 0u);
    EXPECT_FALSE(r.ok());

    BitWriter s;
    s.writeVarU32(1000000); // claims a 1 MB string that isn't there
    BitReader rs(s.data());
    EXPECT_TRUE(rs.readString().empty());
    EXPECT_FALSE(rs.ok());
}

TEST(BitStream, RewindDropsBits) {
    BitWriter w;
    w.writeBits(0b101, 3);
    const u32 mark = w.bitPosition();
    w.writeU32(0xFFFFFFFF);
    w.rewind(mark);
    w.writeBits(0b11, 2);
    BitReader r(w.data());
    EXPECT_EQ(r.readBits(3), 0b101u);
    EXPECT_EQ(r.readBits(2), 0b11u);
    EXPECT_EQ(r.readBits(3), 0u); // padding was cleared by rewind
    EXPECT_EQ(w.bytesWritten(), 1u);
}

TEST(BitStream, QuantizedFloatErrorBound) {
    std::mt19937 rng(42);
    std::uniform_real_distribution<f32> dist(-100.f, 100.f);
    for (f32 resolution : {1.f, 0.1f, 0.01f, 0.001f}) {
        f32 maxErr = 0.f;
        for (int i = 0; i < 2000; ++i) {
            const f32 v = dist(rng);
            BitWriter w;
            w.writeQuantizedFloat(v, -100.f, 100.f, resolution);
            BitReader r(w.data());
            maxErr = std::max(maxErr, std::abs(r.readQuantizedFloat(-100.f, 100.f, resolution) - v));
        }
        EXPECT_LE(maxErr, resolution * 0.5f + 1e-5f) << "resolution " << resolution;
    }
    BitWriter w;
    w.writeQuantizedFloat(5.f, -100.f, 100.f, 0.01f); // 20000 steps -> 15 bits
    EXPECT_EQ(w.bitsWritten(), 15u);

    // Out-of-range values clamp.
    BitWriter c;
    c.writeQuantizedFloat(1000.f, 0.f, 10.f, 0.5f);
    BitReader rc(c.data());
    EXPECT_FLOAT_EQ(rc.readQuantizedFloat(0.f, 10.f, 0.5f), 10.f);
}

TEST(BitStream, QuaternionSmallestThreeError) {
    std::mt19937 rng(7);
    std::normal_distribution<f32> n(0.f, 1.f);
    for (u32 bits : {9u, 10u, 12u, 15u}) {
        f32 maxAngle = 0.f;
        for (int i = 0; i < 2000; ++i) {
            const glm::quat q = glm::normalize(glm::quat(n(rng), n(rng), n(rng), n(rng)));
            BitWriter w;
            w.writeQuat(q, bits);
            EXPECT_EQ(w.bitsWritten(), 2 + 3 * bits);
            BitReader r(w.data());
            const glm::quat d = r.readQuat(bits);
            // Angle via the norm of the difference (well conditioned, unlike acos(dot) near 1).
            const glm::quat dd = glm::dot(q, d) < 0.f ? -d : d;
            const f64 diff = std::sqrt(f64(q.x - dd.x) * (q.x - dd.x) + f64(q.y - dd.y) * (q.y - dd.y) +
                                       f64(q.z - dd.z) * (q.z - dd.z) + f64(q.w - dd.w) * (q.w - dd.w));
            maxAngle = std::max(maxAngle, static_cast<f32>(4.0 * std::asin(std::min(1.0, diff * 0.5))));
        }
        // Per-component step is sqrt(2)/(2^bits-1); the rotation error stays within a few steps.
        const f32 step = 1.41421356f / static_cast<f32>((1u << bits) - 1);
        EXPECT_LT(maxAngle, 4.f * step + 2e-6f) << bits << " bits, max angle " << maxAngle;
    }
}

TEST(BitStream, CodecsRoundTrip) {
    BitWriter w;
    netWrite(w, std::string("abc"));
    netWrite(w, i16{-300});
    netWrite(w, glm::vec4(1, 2, 3, 4));
    netWrite(w, true);
    QuantizedVec3Codec qv{-10.f, 10.f, 0.01f};
    qv.write(w, glm::vec3(1.234f, -5.678f, 9.999f));
    BitReader r(w.data());
    std::string s;
    i16 i = 0;
    glm::vec4 v;
    bool b = false;
    glm::vec3 q;
    netRead(r, s);
    netRead(r, i);
    netRead(r, v);
    netRead(r, b);
    qv.read(r, q);
    EXPECT_EQ(s, "abc");
    EXPECT_EQ(i, -300);
    EXPECT_EQ(v, glm::vec4(1, 2, 3, 4));
    EXPECT_TRUE(b);
    EXPECT_NEAR(q.x, 1.234f, 0.005f);
    EXPECT_NEAR(q.y, -5.678f, 0.005f);
    EXPECT_TRUE(qv.equal(glm::vec3(1.f), glm::vec3(1.001f))); // below resolution -> no resend
    EXPECT_FALSE(qv.equal(glm::vec3(1.f), glm::vec3(1.02f)));
}
