// Глава 02: сериализация в OXB1 и JSON через Writer/Reader (docs/guide/02-reflection-serialization.md).
#include <oxwald/core/reflect.hpp>
#include <oxwald/core/serial/archive.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace {

enum class Difficulty { Easy, Normal, Hard };

struct PlayerData {
    std::string name;
    int level = 1;
    glm::vec3 position{0.0f};
    glm::quat rotation{1, 0, 0, 0};
    std::map<std::string, int> inventory;
    Difficulty difficulty = Difficulty::Normal;
    friend bool operator==(const PlayerData&, const PlayerData&) = default;
};

void registerTypes() {
    OX_REFLECT_ENUM(Difficulty, "guide.Difficulty")
        .value("Easy", Difficulty::Easy)
        .value("Normal", Difficulty::Normal)
        .value("Hard", Difficulty::Hard);
    OX_REFLECT_TYPE(PlayerData, "guide.PlayerData")
        .field("name", &PlayerData::name)
        .field("level", &PlayerData::level)
        .field("position", &PlayerData::position)
        .field("rotation", &PlayerData::rotation)
        .field("inventory", &PlayerData::inventory)
        .field("difficulty", &PlayerData::difficulty);
}

PlayerData makePlayer() {
    PlayerData p;
    p.name = "Ivan";
    p.level = 7;
    p.position = {12.5f, 0.0f, -3.0f};
    p.inventory = {{"potion", 3}, {"arrow", 40}};
    p.difficulty = Difficulty::Hard;
    return p;
}

} // namespace

TEST(GuideSerialization, WriteAndReadBinaryAndJson) {
    registerTypes();
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "oxwald_guide_serialize";
    fs::create_directories(dir);

    const PlayerData player = makePlayer();

    // Writer: тип содержимого ("kind") и версия ваших данных.
    ox::serial::Writer w("profile", /*version*/ 1);
    w.value("player", player);                      // любой отражённый тип
    w.value("playTimeSeconds", 3600.0);             // или встроенный
    w.beginObject("quests");                        // ручная структура
    w.value("active", std::vector<std::string>{"find_sword", "save_village"});
    w.value("completed", 12);
    w.endObject();
    ASSERT_TRUE(w.save(dir / "profile.oxsave"));      // двоичный OXB1
    ASSERT_TRUE(w.save(dir / "profile.oxsave.json")); // суффикс .json => JSON

    for (const char* file : {"profile.oxsave", "profile.oxsave.json"}) {
        auto r = ox::serial::Reader::load(dir / file); // формат определяется по содержимому
        ASSERT_TRUE(r) << r.error().message;
        EXPECT_EQ(r->kind(), "profile");
        EXPECT_EQ(r->version(), 1u);

        PlayerData loaded;
        ASSERT_TRUE(r->value("player", loaded));
        EXPECT_EQ(loaded, player);

        ASSERT_TRUE(r->beginObject("quests"));
        std::vector<std::string> active;
        int completed = 0;
        EXPECT_TRUE(r->value("active", active));
        EXPECT_TRUE(r->value("completed", completed));
        r->endObject();
        EXPECT_EQ(active.size(), 2u);
        EXPECT_EQ(completed, 12);

        int missing = -1;
        EXPECT_FALSE(r->value("noSuchKey", missing)); // нет ключа — false, значение не тронуто
        EXPECT_EQ(missing, -1);
    }
    fs::remove_all(dir);
}

TEST(GuideSerialization, BinaryJsonConversionIsLossless) {
    registerTypes();
    ox::serial::Writer w("profile", 1);
    w.value("player", makePlayer());
    const std::vector<std::byte> binary = w.toBinary();

    // Как oxdump: конвертация без знания C++-типов.
    auto json = ox::serial::binaryToJson(binary);
    ASSERT_TRUE(json) << json.error().message;
    EXPECT_NE(json->find("\"difficulty\": \"Hard\""), std::string::npos); // enum в JSON — по имени
    EXPECT_NE(json->find("\"$type\": \"guide.PlayerData\""), std::string::npos);

    auto back = ox::serial::jsonToBinary(*json);
    ASSERT_TRUE(back);
    EXPECT_EQ(*back, binary); // байт в байт

    // Структура файла: заголовок, чанки с CRC32, таблица схемы.
    auto info = ox::serial::inspectBinary(binary);
    ASSERT_TRUE(info);
    EXPECT_EQ(info->kind, "profile");
    for (const auto& chunk : info->chunks) EXPECT_TRUE(chunk.crcValid) << chunk.id;
}

TEST(GuideSerialization, HandWrittenJson) {
    registerTypes();
    // JSON можно написать руками (без "schema"/"$type"): типы выводятся, конверсия — при чтении в C++-тип.
    const char* text = R"({
        "player": { "name": "Bot", "level": 5, "position": [1, 2, 3], "difficulty": "Easy" }
    })";
    auto doc = ox::serial::parseJsonString(text);
    ASSERT_TRUE(doc) << doc.error().message;
    PlayerData p;
    ASSERT_TRUE(ox::serial::Reader(*doc).value("player", p));
    EXPECT_EQ(p.name, "Bot");
    EXPECT_EQ(p.position, glm::vec3(1, 2, 3));
    EXPECT_EQ(p.difficulty, Difficulty::Easy);
}
