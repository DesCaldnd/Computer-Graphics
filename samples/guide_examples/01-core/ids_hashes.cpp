// Глава 01: UUID и хэши (docs/guide/01-core.md).
#include <oxwald/core/hash.hpp>
#include <oxwald/core/uuid.hpp>

#include <gtest/gtest.h>

#include <string>
#include <unordered_map>

using namespace ox::literals; // "..."_hash

namespace {
// Хэш строки считается при компиляции — годится для switch.
std::string describe(ox::u64 eventId) {
    switch (eventId) {
    case "player.died"_hash: return "died";
    case "player.spawned"_hash: return "spawned";
    default: return "unknown";
    }
}
} // namespace

TEST(GuideCoreIds, UuidBasics) {
    const ox::Uuid id = ox::Uuid::generate(); // случайный v4
    EXPECT_TRUE(id.isValid());

    const std::string text = id.toString(); // "8-4-4-4-12"
    EXPECT_EQ(ox::Uuid::parse(text), id);
    EXPECT_FALSE(ox::Uuid::parse("not-a-uuid"));

    // Детерминированный UUID из имени — удобно для встроенных ассетов и тестов.
    EXPECT_EQ(ox::Uuid::fromName("builtin/cube"), ox::Uuid::fromName("builtin/cube"));
    EXPECT_TRUE(ox::Uuid{}.isNil()); // нулевой UUID = «нет ссылки»

    std::unordered_map<ox::Uuid, std::string> names; // std::hash<Uuid> есть
    names[id] = "Player";
    EXPECT_EQ(names.at(id), "Player");
}

TEST(GuideCoreIds, Hashes) {
    EXPECT_EQ(describe(ox::hashString("player.died")), "died");
    EXPECT_EQ(describe(ox::fnv1a64("enemy.died")), "unknown");

    ox::u64 key = 0;
    ox::hashCombineInto(key, std::string("Shadows"));
    ox::hashCombineInto(key, 2048);
    EXPECT_NE(key, 0u);

    const char data[] = "123456789";
    EXPECT_EQ(ox::crc32(data, 9), 0xCBF43926u); // эталонное значение CRC-32/IEEE
}
