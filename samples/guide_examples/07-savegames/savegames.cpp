// Глава 07: сохранения — атрибут SaveGame, SaveGameComponent, ISaveable, слоты, миграции, async, автосейв,
// дамп в JSON (docs/guide/07-savegames.md).
#include <oxwald/core/jobs.hpp>
#include <oxwald/core/serial/format.hpp>
#include <oxwald/runtime/save_game.hpp>
#include <oxwald/scene/scene.hpp>
#include <oxwald/scene/scene_serializer.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

// ---- компоненты игры ----
struct HealthComponent {
    ox::f32 health = 100.0f;    // меняется в игре -> сохраняем
    ox::f32 maxHealth = 100.0f; // данные уровня -> берём из сцены
};

struct InventoryComponent {
    std::vector<std::string> items;
    ox::i32 gold = 0;
};

void registerGameTypes() {
    ox::registerSceneTypes();
    ox::registerSaveGameTypes();
    OX_REFLECT_TYPE(HealthComponent, "Game.Health")
        .field("health", &HealthComponent::health, ox::attr::SaveGame{})
        .field("maxHealth", &HealthComponent::maxHealth);
    OX_REFLECT_TYPE(InventoryComponent, "Game.Inventory")
        .field("items", &InventoryComponent::items)
        .field("gold", &InventoryComponent::gold);
    ox::ComponentRegistry::instance().add<HealthComponent>();
    ox::ComponentRegistry::instance().add<InventoryComponent>();
}

// ---- данные вне ECS: журнал квестов ----
class QuestLog final : public ox::ISaveable {
public:
    std::string saveId() const override { return "quests"; }
    void save(ox::serial::Writer& w) const override {
        w.value("active", active);
        w.value("completed", completed);
    }
    bool load(ox::serial::Reader& r) override {
        active.clear();
        completed = 0;
        r.value("active", active);
        r.value("completed", completed);
        return true;
    }
    void onMissing() override { *this = {}; } // старое сохранение без этой секции

    std::vector<std::string> active;
    ox::i32 completed = 0;
};

// Уровень «как на диске»: игрок сохраняется целиком, у двери — только SaveGame-поля.
ox::serial::Document makeLevel() {
    ox::World w;
    ox::Entity player = w.create("Player");
    player.add<ox::SaveGameComponent>(); // вся сущность целиком (все компоненты + трансформ)
    player.add<HealthComponent>();
    player.add<InventoryComponent>();
    ox::Entity door = w.create("Door");
    door.add<HealthComponent>().health = 50.0f; // сохранится только health
    w.create("Crate");                           // обычная сущность уровня
    return ox::serializeWorld(w);
}

std::unique_ptr<ox::World> loadLevel(const ox::serial::Document& level) {
    auto w = std::make_unique<ox::World>();
    EXPECT_TRUE(ox::deserializeWorld(*w, level));
    return w;
}

struct TempDir {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("oxwald_guide_saves_" + ox::Uuid::generate().toString());
    ~TempDir() { std::filesystem::remove_all(path); }
};

ox::SaveGameConfig makeConfig(const TempDir& tmp) {
    ox::SaveGameConfig config;
    config.directory = tmp.path / "saves"; // в Engine: user://saves
    config.version = 1;                    // версия данных сохранений (ProjectSettings::saveVersion)
    config.gameVersion = "1.0.0";
    return config;
}

} // namespace

TEST(GuideSaveGame, SaveAndLoadSlot) {
    registerGameTypes();
    TempDir tmp;
    ox::SaveGameSystem saves(makeConfig(tmp));
    QuestLog quests;
    saves.registerSaveable(quests);

    const ox::serial::Document level = makeLevel();
    auto world = loadLevel(level);
    saves.setCurrentLevel("project://levels/forest.oxscene");
    saves.trackLevelEntities(*world); // запомнить сущности уровня, чтобы сохранить факт их уничтожения

    // Играем.
    ox::Entity player = world->findByName("Player");
    player.get<HealthComponent>().health = 42.0f;
    player.get<InventoryComponent>().items = {"sword"};
    player.setPosition({3, 0, 7});
    world->findByName("Door").get<HealthComponent>().health = 0.0f;
    world->findByName("Door").get<HealthComponent>().maxHealth = 999.0f; // не SaveGame-поле
    world->destroyImmediate(world->findByName("Crate"));                  // ящик разбит
    ox::Entity coin = world->create("Coin");                               // заспавнен в игре
    coin.add<ox::SaveGameComponent>();
    quests.active = {"FindTheSword"};

    auto saved = saves.save("slot1", *world, ox::SaveKind::Manual, "У старого моста");
    ASSERT_TRUE(saved) << saved.error().message;

    // Перезапуск: уровень грузится заново, сохранение накладывается поверх.
    quests = {};
    auto fresh = loadLevel(level);
    auto loaded = saves.load("slot1", *fresh);
    ASSERT_TRUE(loaded) << loaded.error().message;
    EXPECT_EQ(loaded->header.displayName, "У старого моста");
    EXPECT_EQ(loaded->entitiesCreated, 1u);   // Coin
    EXPECT_EQ(loaded->entitiesDestroyed, 1u); // Crate

    ox::Entity p = fresh->findByName("Player");
    EXPECT_EQ(p.get<HealthComponent>().health, 42.0f);
    EXPECT_EQ(p.get<InventoryComponent>().items, std::vector<std::string>{"sword"});
    EXPECT_EQ(p.localTransform().position, glm::vec3(3, 0, 7));
    ox::Entity door = fresh->findByName("Door");
    EXPECT_EQ(door.get<HealthComponent>().health, 0.0f);
    EXPECT_EQ(door.get<HealthComponent>().maxHealth, 100.0f); // значение из уровня
    EXPECT_FALSE(fresh->findByName("Crate").valid());
    EXPECT_TRUE(fresh->findByName("Coin").valid());
    EXPECT_EQ(quests.active, std::vector<std::string>{"FindTheSword"});
}

TEST(GuideSaveGame, SlotsMenu) {
    registerGameTypes();
    TempDir tmp;
    ox::SaveGameSystem saves(makeConfig(tmp));
    saves.setThumbnailProvider([] { return std::vector<std::byte>(16, std::byte{0x89}); }); // обычно PNG кадра
    ox::World world;
    world.create("Hero").add<ox::SaveGameComponent>();
    saves.setPlayTime(3600.0);
    ASSERT_TRUE(saves.save("slot1", world, ox::SaveKind::Manual, "Глава 1"));

    for (const ox::SaveSlotInfo& slot : saves.listSlots()) { // новые первыми
        EXPECT_EQ(slot.header.slot, "slot1");
        EXPECT_EQ(slot.header.displayName, "Глава 1");
        EXPECT_EQ(slot.header.playTimeSeconds, 3600.0);
        EXPECT_TRUE(slot.hasThumbnail);
        EXPECT_FALSE(slot.corrupted);
    }
    EXPECT_TRUE(saves.exists("slot1"));
    EXPECT_FALSE(ox::SaveGameSystem::isValidSlotName("../hack")); // только безопасные имена
    EXPECT_TRUE(saves.deleteSlot("slot1"));
}

TEST(GuideSaveGame, Migrations) {
    registerGameTypes();
    TempDir tmp;
    ox::World world;
    world.create("Hero").add<ox::SaveGameComponent>();
    {
        // Версия 1 игры: квесты хранили счётчик под именем "done".
        ox::SaveGameSystem v1(makeConfig(tmp));
        struct OldQuests final : ox::ISaveable {
            std::string saveId() const override { return "quests"; }
            void save(ox::serial::Writer& w) const override { w.value("done", ox::i32{7}); }
            bool load(ox::serial::Reader&) override { return true; }
        } old;
        v1.registerSaveable(old);
        ASSERT_TRUE(v1.save("old", world));
    }

    // Версия 2: поле переименовано в "completed". Миграция 1 -> 2 правит дерево значений до загрузки.
    auto config = makeConfig(tmp);
    config.version = 2;
    ox::SaveGameSystem saves(config);
    QuestLog quests;
    saves.registerSaveable(quests);
    saves.registerMigration(1, [](ox::serial::Document& doc) -> ox::Status {
        ox::serial::Value* q = doc.root.find("sections")->find("quests");
        if (!q) return {}; // секции может не быть
        if (const ox::serial::Value* done = q->find("done")) {
            q->set("completed", ox::serial::Value::makeInt(done->getInt(), ox::serial::Tag::I32));
            q->erase("done");
        }
        return {};
    });

    ox::World fresh;
    auto r = saves.load("old", fresh);
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(r->migratedFrom, 1u);
    EXPECT_EQ(quests.completed, 7);
}

TEST(GuideSaveGame, AsyncAndAutosave) {
    registerGameTypes();
    TempDir tmp;
    ox::JobSystem jobs(2);
    ox::SaveGameSystem saves(makeConfig(tmp), &jobs);
    ox::World world;
    ox::Entity hero = world.create("Hero");
    hero.add<ox::SaveGameComponent>();
    hero.add<HealthComponent>().health = 10.0f;
    saves.setWorldProvider([&] { return &world; }); // откуда брать мир для автосейва/асинхронной загрузки

    // Снимок берётся сразу, кодирование и запись — в фоне. Мир можно менять сразу после вызова.
    ox::SaveHandle handle = saves.saveAsync("async", world);
    hero.get<HealthComponent>().health = 99.0f;
    handle->wait();
    ASSERT_TRUE(handle->result()) << handle->result().error().message;

    // Асинхронная загрузка: чтение/декодирование в фоне, применение — в update() на игровом потоке.
    ox::LoadHandle load = saves.loadAsync("async");
    for (int i = 0; i < 1000 && !load->done(); ++i) {
        saves.update(0.0); // Engine вызывает это каждый кадр сам
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(load->done());
    ASSERT_TRUE(load->result());
    EXPECT_EQ(hero.get<HealthComponent>().health, 10.0f); // состояние на момент снимка

    // Автосейв каждые 60 с игрового времени, по кругу autosave0..autosave2.
    saves.setAutosaveInterval(60.0);
    for (int i = 0; i < 61; ++i) saves.update(1.0, /*playing*/ true); // меню и пауза время не копят: playing = false
    saves.waitIdle();
    EXPECT_TRUE(saves.exists("autosave0"));
}

TEST(GuideSaveGame, ExportJson) {
    registerGameTypes();
    TempDir tmp;
    ox::SaveGameSystem saves(makeConfig(tmp));
    ox::World world;
    ox::Entity hero = world.create("Hero");
    hero.add<ox::SaveGameComponent>();
    hero.add<HealthComponent>().health = 33.0f;
    ASSERT_TRUE(saves.save("debug", world));

    // То же, что `oxdump debug.oxsave -o debug.oxsave.json`.
    auto json = saves.exportJson("debug", tmp.path / "debug.oxsave.json");
    ASSERT_TRUE(json) << json.error().message;
    EXPECT_NE(json->find("Game.Health"), std::string::npos);

    // Обратно в бинарный — байт в байт (`oxdump --to-binary`): можно править сохранение руками.
    auto binary = ox::serial::jsonToBinary(*json);
    ASSERT_TRUE(binary);
    EXPECT_EQ(*binary, *ox::serial::readFileBytes(saves.slotPath("debug")));
}
