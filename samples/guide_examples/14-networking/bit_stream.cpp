// Глава 14: побитовая упаковка (BitWriter/BitReader), квантование и собственный NetCodec
// (docs/guide/14-networking.md).
#include <oxwald/net/bit_stream.hpp>
#include <oxwald/net/serialize.hpp>

#include <glm/gtc/quaternion.hpp>
#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace ox;
using namespace ox::net;

// Состояние игрока в снапшоте: ~13 байт вместо 40+ «как есть».
struct PlayerState {
    u8 team = 0;           // 0..3
    f32 health = 100.f;    // 0..100, шаг 0.1
    glm::vec3 position{0.f};
    glm::quat rotation{1.f, 0.f, 0.f, 0.f};
    bool crouching = false;
};

void writePlayer(BitWriter& w, const PlayerState& p) {
    w.writeRangedInt(p.team, 0, 3);                            // 2 бита
    w.writeQuantizedFloat(p.health, 0.f, 100.f, 0.1f);        // 10 бит, ошибка <= 0.05
    w.writeQuantizedVec3(p.position, -1024.f, 1024.f, 0.01f); // 18 бит на ось
    w.writeQuat(p.rotation, 10);                               // 32 бита («smallest three»)
    w.writeBool(p.crouching);                                  // 1 бит
}

PlayerState readPlayer(BitReader& r) {
    PlayerState p;
    p.team = static_cast<u8>(r.readRangedInt(0, 3));
    p.health = r.readQuantizedFloat(0.f, 100.f, 0.1f);
    p.position = r.readQuantizedVec3(-1024.f, 1024.f, 0.01f);
    p.rotation = r.readQuat(10);
    p.crouching = r.readBool();
    return p;
}

// Собственный тип в RPC/репликации: специализация NetCodec (write/read/equal).
struct ItemStack {
    u16 itemId = 0;
    u8 count = 0;
};

template <>
struct ox::net::NetCodec<ItemStack> {
    void write(BitWriter& w, const ItemStack& v) const {
        w.writeVarU32(v.itemId);
        w.writeRangedInt(v.count, 0, 99);
    }
    void read(BitReader& r, ItemStack& v) const {
        v.itemId = static_cast<u16>(r.readVarU32());
        v.count = static_cast<u8>(r.readRangedInt(0, 99));
    }
    bool equal(const ItemStack& a, const ItemStack& b) const { return a.itemId == b.itemId && a.count == b.count; }
};

TEST(GuideNetBitStream, PackPlayerState) {
    PlayerState in;
    in.team = 2;
    in.health = 73.46f;
    in.position = {12.345f, -3.5f, 900.f};
    in.rotation = glm::angleAxis(0.7f, glm::normalize(glm::vec3(0.f, 1.f, 0.2f)));
    in.crouching = true;

    BitWriter w;
    writePlayer(w, in);
    EXPECT_EQ(w.bitsWritten(), 2u + 10u + 3u * 18u + 32u + 1u);
    EXPECT_EQ(w.bytesWritten(), 13u);

    BitReader r(w.data());
    PlayerState out = readPlayer(r);
    ASSERT_TRUE(r.ok()); // одна проверка после пачки чтений
    EXPECT_EQ(out.team, 2);
    EXPECT_NEAR(out.health, 73.46f, 0.05f);
    EXPECT_NEAR(out.position.x, 12.345f, 0.005f);
    EXPECT_LT(std::abs(1.f - std::abs(glm::dot(out.rotation, in.rotation))), 1e-4f);
    EXPECT_TRUE(out.crouching);
}

TEST(GuideNetBitStream, TruncatedPacketIsDetected) {
    BitWriter w;
    w.writeU32(7);
    std::vector<u8> bytes(w.data().begin(), w.data().end());
    bytes.resize(2); // «обрезанный» пакет от злоумышленника
    BitReader r(bytes);
    (void)r.readU32();
    EXPECT_FALSE(r.ok()); // чтение за концом не падает: нули + флаг ошибки
}

TEST(GuideNetBitStream, CustomCodecAndVarints) {
    BitWriter w;
    netWrite(w, ItemStack{1234, 42});
    netWrite(w, std::string("Меч"));
    w.writeVarU32(5); // маленькие числа — 1 байт
    BitReader r(w.data());
    ItemStack stack;
    std::string name;
    netRead(r, stack);
    netRead(r, name);
    EXPECT_EQ(stack.itemId, 1234);
    EXPECT_EQ(stack.count, 42);
    EXPECT_EQ(name, "Меч");
    EXPECT_EQ(r.readVarU32(), 5u);
    EXPECT_TRUE(r.ok());
}
