// Глава 02: эволюция формата — новые, удалённые, переименованные поля и миграции по версии
// (docs/guide/02-reflection-serialization.md).
#include <oxwald/core/reflect.hpp>
#include <oxwald/core/serial/archive.hpp>

#include <gtest/gtest.h>

#include <string>

// В реальном проекте это одна и та же структура до и после правки; здесь обе версии живут рядом.
namespace v1 {
struct Character {
    std::string playerName = "anon";
    int level = 1;
    float health = 1.0f;       // в v1 — доля 0..1
    int legacyKarma = 0;       // в v2 поле удалено
};
} // namespace v1

namespace v2 {
struct Character {
    std::string displayName;   // переименовано из playerName
    double level = 1.0;        // тип расширен: int -> double
    float health = 100.0f;     // в v2 — абсолютные единицы 0..100
    bool hardcore = false;     // новое поле
};
} // namespace v2

namespace {

constexpr ox::u32 kCharacterVersion = 2; // версия ваших данных (не формата OXB1)

void registerTypes() {
    OX_REFLECT_TYPE(v1::Character, "guide.CharacterV1")
        .field("playerName", &v1::Character::playerName)
        .field("level", &v1::Character::level)
        .field("health", &v1::Character::health)
        .field("legacyKarma", &v1::Character::legacyKarma);

    OX_REFLECT_TYPE(v2::Character, "guide.Character")
        .field("displayName", &v2::Character::displayName, ox::attr::FormerName{"playerName"})
        .field("level", &v2::Character::level)
        .field("health", &v2::Character::health)
        .field("hardcore", &v2::Character::hardcore);
}

// Миграция смысла данных: v1 -> v2 (здоровье из доли в абсолютные единицы). Правим дерево значений
// документа до чтения в C++-тип — так же устроены миграции сохранений (SaveGameSystem::registerMigration).
ox::Status migrate(ox::serial::Document& doc) {
    if (doc.version < 2) {
        ox::serial::Value* character = doc.root.find("character");
        if (character == nullptr) return ox::makeError("no 'character' in {} v{}", doc.kind, doc.version);
        if (ox::serial::Value* hp = character->find("health")) {
            *hp = ox::serial::Value::makeF32(static_cast<float>(hp->getDouble() * 100.0));
        }
        doc.version = 2;
    }
    return {};
}

ox::Result<v2::Character> loadCharacter(std::span<const std::byte> bytes) {
    auto doc = ox::serial::decodeAny(bytes);
    if (!doc) return doc.error();
    if (doc->version > kCharacterVersion) return ox::makeError("file is from a newer game version");
    if (auto st = migrate(*doc); !st) return st.error();
    v2::Character c;
    ox::serial::Reader(std::move(*doc)).value("character", c);
    return c;
}

} // namespace

TEST(GuideVersioning, OldFileLoadsIntoNewType) {
    registerTypes();
    v1::Character old;
    old.playerName = "Oxwald";
    old.level = 8;
    old.health = 0.75f;
    old.legacyKarma = 42;

    ox::serial::Writer w("character", /*version*/ 1);
    w.value("character", old);
    const auto bytes = w.toBinary();

    auto loaded = loadCharacter(bytes);
    ASSERT_TRUE(loaded) << loaded.error().message;
    EXPECT_EQ(loaded->displayName, "Oxwald");  // переименование через FormerName
    EXPECT_DOUBLE_EQ(loaded->level, 8.0);       // int -> double
    EXPECT_FLOAT_EQ(loaded->health, 75.0f);     // миграция по версии
    EXPECT_FALSE(loaded->hardcore);             // нового поля в файле нет — значение по умолчанию
    // legacyKarma просто пропущено.
}

TEST(GuideVersioning, NewFileIsReadableByOldBuild) {
    registerTypes();
    v2::Character fresh;
    fresh.displayName = "New";
    fresh.level = 3.9;
    fresh.hardcore = true;
    ox::serial::Writer w("character", kCharacterVersion);
    w.value("character", fresh);

    // Старая сборка читает новый файл: незнакомые поля пропускаются по размеру записи.
    v1::Character old;
    auto r = ox::serial::Reader::fromBytes(w.toBinary());
    ASSERT_TRUE(r);
    ASSERT_TRUE(r->value("character", old));
    EXPECT_EQ(old.level, 3);           // double -> int (усечение)
    EXPECT_EQ(old.playerName, "anon"); // старая сборка не знает нового имени поля
}
