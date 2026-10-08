// Глава 02: регистрация типов в рефлексии, атрибуты, обход полей, ValueRef (docs/guide/02-reflection-serialization.md).
#include <oxwald/core/reflect.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace game {

enum class WeaponKind : ox::u8 { Sword, Bow, Staff };

struct WeaponStats {
    float damage = 10.0f;
    float range = 1.5f;
};

struct Weapon {
    std::string name = "Rusty sword";
    WeaponKind kind = WeaponKind::Sword;
    WeaponStats stats;
    glm::vec3 tint{1.0f};
    std::vector<std::string> tags;
    std::optional<ox::Uuid> icon; // ссылка на ассет-текстуру (может отсутствовать)
    int durability = 100;
    float cachedDps = 0.0f; // вычисляется в рантайме, не сохраняется
};

// Регистрация — явной функцией: статические инициализаторы в статических библиотеках выкидывает линкер.
void registerGameTypes() {
    OX_REFLECT_ENUM(WeaponKind, "game.WeaponKind")
        .value("Sword", WeaponKind::Sword)
        .value("Bow", WeaponKind::Bow)
        .value("Staff", WeaponKind::Staff, ox::attr::DisplayName{"Magic staff"});

    OX_REFLECT_TYPE(WeaponStats, "game.WeaponStats")
        .field("damage", &WeaponStats::damage, ox::attr::Range{0.0, 1000.0}, ox::attr::Step{0.5})
        .field("range", &WeaponStats::range, ox::attr::Range{0.0, 100.0}, ox::attr::Tooltip{"Metres"});

    OX_REFLECT_TYPE(Weapon, "game.Weapon")
        .attributes(ox::attr::Category{"Gameplay"}, ox::attr::Meta{"icon", "sword"})
        .field("name", &Weapon::name, ox::attr::DisplayName{"Display name"})
        .field("kind", &Weapon::kind)
        .field("stats", &Weapon::stats)
        .field("tint", &Weapon::tint, ox::attr::Color{})
        .field("tags", &Weapon::tags)
        .field("icon", &Weapon::icon, ox::attr::AssetRef{"Texture"})
        .field("durability", &Weapon::durability, ox::attr::SaveGame{}, ox::attr::Replicated{})
        .field("cachedDps", &Weapon::cachedDps, ox::attr::NoSerialize{}, ox::attr::ReadOnly{});
}

} // namespace game

TEST(GuideReflection, TypeInfoFieldsAndAttributes) {
    game::registerGameTypes();
    game::registerGameTypes(); // повторная регистрация безопасна (идемпотентна)

    const ox::reflect::TypeInfo& type = ox::reflect::typeOf<game::Weapon>();
    EXPECT_EQ(type.name, "game.Weapon");
    EXPECT_EQ(type.attributes.category, "Gameplay");
    EXPECT_EQ(type.attributes.getMeta("icon"), "sword");
    ASSERT_EQ(type.fields.size(), 8u);

    // Обход полей — так работают инспектор редактора, сериализация, сеть и Lua.
    std::vector<std::string> saved;
    for (const ox::reflect::FieldInfo& f : type.fields) {
        if (f.attributes.saveGame) saved.push_back(f.name);
    }
    EXPECT_EQ(saved, std::vector<std::string>{"durability"});

    const auto* damage = ox::reflect::typeOf<game::WeaponStats>().findField("damage");
    ASSERT_NE(damage, nullptr);
    EXPECT_EQ(*damage->attributes.rangeMax, 1000.0);
    EXPECT_EQ(type.findField("name")->displayName(), "Display name");
    EXPECT_EQ(type.findField("icon")->attributes.assetType, "Texture");

    // Типы можно найти по имени (например, при загрузке данных).
    EXPECT_EQ(ox::reflect::TypeRegistry::instance().find("game.Weapon"), &type);

    // Перечисления: имя <-> значение.
    const auto& kind = ox::reflect::typeOf<game::WeaponKind>();
    EXPECT_EQ(kind.findEnum("Bow")->value, 1);
    EXPECT_EQ(kind.findEnumByValue(2)->attributes.displayName, "Magic staff");
}

TEST(GuideReflection, GenericAccessByPath) {
    game::registerGameTypes();
    game::Weapon bow;
    bow.kind = game::WeaponKind::Bow;
    bow.tags = {"ranged", "wood"};

    auto root = ox::reflect::ValueRef::of(bow);
    // Пути как в инспекторе и undo/redo: поля, компоненты векторов, индексы массивов.
    EXPECT_TRUE(ox::reflect::resolvePath(root, "stats.range").setAs(25.0)); // double -> float, конверсия терпимая
    EXPECT_FLOAT_EQ(bow.stats.range, 25.0f);
    EXPECT_TRUE(ox::reflect::resolvePath(root, "tint.g").setAs(0.5f));
    EXPECT_FLOAT_EQ(bow.tint.g, 0.5f);
    EXPECT_EQ(ox::reflect::resolvePath(root, "tags[1]").getAs<std::string>(), "wood");

    // Перечисление читается как значение дерева serial::Value — по имени.
    EXPECT_EQ(ox::reflect::resolvePath(root, "kind").get().getString(), "Bow");
    EXPECT_FALSE(ox::reflect::resolvePath(root, "no.such.field"));
}
