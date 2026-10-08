#include "test_util.hpp"

#include <oxwald/core/jobs.hpp>
#include <oxwald/core/reflect.hpp>
#include <oxwald/core/serial/format.hpp>
#include <oxwald/runtime/engine.hpp>
#include <oxwald/runtime/save_game.hpp>
#include <oxwald/scene/scene.hpp>
#include <oxwald/scene/scene_serializer.hpp>

#include <gtest/gtest.h>

#include <fstream>
#include <thread>

using namespace ox;

namespace {

struct HealthComponent {
    f32 health = 100.0f;    // SaveGame
    f32 maxHealth = 100.0f; // level data, not saved
    EntityRef target;       // SaveGame
};

struct InventoryComponent {
    std::vector<std::string> items;
    i32 gold = 0;
};

void registerTestTypes() {
    registerSaveGameTypes();
    OX_REFLECT_TYPE(HealthComponent, "Test.Health")
        .field("health", &HealthComponent::health, attr::SaveGame{})
        .field("maxHealth", &HealthComponent::maxHealth)
        .field("target", &HealthComponent::target, attr::SaveGame{});
    OX_REFLECT_TYPE(InventoryComponent, "Test.Inventory")
        .field("items", &InventoryComponent::items)
        .field("gold", &InventoryComponent::gold);
    ComponentRegistry::instance().add<HealthComponent>();
    ComponentRegistry::instance().add<InventoryComponent>();
}

class QuestLog final : public ISaveable {
public:
    std::string saveId() const override { return "quests"; }
    void save(serial::Writer& w) const override {
        w.value("active", active);
        w.value("gold", gold);
        w.beginObject("meta");
        w.value("chapter", chapter);
        w.endObject();
    }
    bool load(serial::Reader& r) override {
        loaded = true;
        r.value("active", active);
        r.value("gold", gold);
        r.value("coins", coins); // added by a migration
        if (r.beginObject("meta")) {
            r.value("chapter", chapter);
            r.endObject();
        }
        return true;
    }
    void onMissing() override { missing = true; }

    std::vector<std::string> active;
    i32 gold = 0;
    i32 coins = 0;
    i32 chapter = 0;
    bool loaded = false;
    bool missing = false;
};

struct Fixture {
    test::TempDir dir{"saves"};
    SaveGameConfig config() const {
        SaveGameConfig c;
        c.directory = dir / "saves";
        c.gameVersion = "1.0.0";
        c.version = 1;
        c.maxAutosaves = 3;
        return c;
    }
};

// Level as stored on disk: Player (whole entity), Door (only SaveGame fields), Crate (whole), Static (plain).
serial::Document makeLevel() {
    World w;
    Entity player = w.create("Player");
    player.add<SaveGameComponent>();
    player.add<HealthComponent>();
    player.add<InventoryComponent>().items = {"sword"};
    player.setPosition({0, 1, 0});
    Entity door = w.create("Door");
    door.add<HealthComponent>().health = 50.0f;
    w.create("Crate").add<SaveGameComponent>();
    w.create("Static").setPosition({5, 0, 5});
    return serializeWorld(w);
}

std::unique_ptr<World> loadLevel(const serial::Document& level) {
    auto w = std::make_unique<World>();
    EXPECT_TRUE(deserializeWorld(*w, level));
    return w;
}

} // namespace

TEST(SaveGame, RestoresSaveGameFieldsAndEntities) {
    registerTestTypes();
    Fixture f;
    SaveGameSystem saves(f.config());
    QuestLog quests;
    saves.registerSaveable(quests);

    const serial::Document level = makeLevel();
    auto world = loadLevel(level);
    saves.setCurrentLevel("project://levels/test.oxscene");
    saves.trackLevelEntities(*world);
    saves.setPlayTime(123.5);

    Entity player = world->findByName("Player");
    Entity door = world->findByName("Door");
    const Uuid playerId = player.uuid(), doorId = door.uuid();
    player.get<HealthComponent>().health = 42.0f;
    player.get<HealthComponent>().maxHealth = 500.0f; // whole entity: everything is saved
    player.get<InventoryComponent>().items.push_back("shield");
    player.setPosition({3, 4, 5});
    door.get<HealthComponent>().health = 10.0f;
    door.get<HealthComponent>().maxHealth = 777.0f; // not a SaveGame field
    door.get<HealthComponent>().target = player.ref();
    Entity pickup = world->create("Pickup", player); // spawned at runtime, child of the player
    pickup.add<SaveGameComponent>().saveTransform = true;
    pickup.add<InventoryComponent>().items = {"gem"};
    pickup.setPosition({0, 2, 0});
    const Uuid pickupId = pickup.uuid();
    world->destroyImmediate(world->findByName("Crate"));
    world->destroyImmediate(world->findByName("Static"));
    quests.active = {"FindTheGem"};
    quests.gold = 99;
    quests.chapter = 2;

    auto saved = saves.save("slot1", *world, SaveKind::Manual, "Before the boss");
    ASSERT_TRUE(saved) << saved.error().message;
    EXPECT_GT(saved->bytes, 0u);

    // Fresh level (as after a restart) + load.
    quests = {};
    saves.setPlayTime(0.0);
    auto fresh = loadLevel(level);
    auto r = saves.load("slot1", *fresh);
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_FALSE(r->fromBackup);
    EXPECT_EQ(r->header.displayName, "Before the boss");
    EXPECT_EQ(r->header.level, "project://levels/test.oxscene");
    EXPECT_EQ(r->header.gameVersion, "1.0.0");
    EXPECT_DOUBLE_EQ(r->header.playTimeSeconds, 123.5);
    EXPECT_DOUBLE_EQ(saves.playTime(), 123.5);
    EXPECT_EQ(r->entitiesCreated, 1u);   // Pickup
    EXPECT_EQ(r->entitiesDestroyed, 2u); // Crate (SaveGame entity missing from the save), Static (level entity)

    Entity p = fresh->find(playerId);
    ASSERT_TRUE(p.valid());
    EXPECT_FLOAT_EQ(p.get<HealthComponent>().health, 42.0f);
    EXPECT_FLOAT_EQ(p.get<HealthComponent>().maxHealth, 500.0f);
    EXPECT_EQ(p.get<InventoryComponent>().items, (std::vector<std::string>{"sword", "shield"}));
    EXPECT_EQ(p.localTransform().position, glm::vec3(3, 4, 5));
    Entity d = fresh->find(doorId);
    ASSERT_TRUE(d.valid());
    EXPECT_FLOAT_EQ(d.get<HealthComponent>().health, 10.0f);
    EXPECT_FLOAT_EQ(d.get<HealthComponent>().maxHealth, 100.0f); // level value
    EXPECT_EQ(fresh->resolve(d.get<HealthComponent>().target), p); // EntityRef resolves after restore
    Entity pk = fresh->find(pickupId);
    ASSERT_TRUE(pk.valid());
    EXPECT_EQ(pk.name(), "Pickup");
    EXPECT_EQ(pk.parent(), p);
    EXPECT_EQ(pk.get<InventoryComponent>().items, std::vector<std::string>{"gem"});
    EXPECT_EQ(pk.worldPosition(), glm::vec3(3, 6, 5));
    EXPECT_FALSE(fresh->findByName("Crate").valid());
    EXPECT_FALSE(fresh->findByName("Static").valid());

    EXPECT_TRUE(quests.loaded);
    EXPECT_EQ(quests.active, std::vector<std::string>{"FindTheGem"});
    EXPECT_EQ(quests.gold, 99);
    EXPECT_EQ(quests.chapter, 2);

    // Loading onto a world that kept changing: entities spawned after the save are removed, destroyed ones come
    // back, component additions on whole entities are reverted.
    Entity ghost = fresh->create("Ghost");
    ghost.add<SaveGameComponent>();
    fresh->destroyImmediate(fresh->find(pickupId));
    p.add<LightComponent>();
    p.get<HealthComponent>().health = 1.0f;
    auto again = saves.load("slot1", *fresh);
    ASSERT_TRUE(again);
    EXPECT_FALSE(fresh->findByName("Ghost").valid());
    EXPECT_TRUE(fresh->find(pickupId).valid());
    EXPECT_FALSE(p.has<LightComponent>());
    EXPECT_FLOAT_EQ(p.get<HealthComponent>().health, 42.0f);
}

TEST(SaveGame, MissingSectionAndSlotQueries) {
    registerTestTypes();
    Fixture f;
    SaveGameSystem saves(f.config());
    saves.setThumbnailProvider([] {
        std::vector<std::byte> png(64);
        for (usize i = 0; i < png.size(); ++i) png[i] = std::byte(i);
        return png;
    });
    World w;
    w.create("A").add<SaveGameComponent>();
    ASSERT_TRUE(saves.save("first", w));
    std::this_thread::sleep_for(std::chrono::milliseconds(1100)); // header timestamps have 1 s resolution
    ASSERT_TRUE(saves.save("second", w, SaveKind::Quick));

    QuestLog quests;
    saves.registerSaveable(quests); // not present in the saves
    ASSERT_TRUE(saves.load("first", w));
    EXPECT_TRUE(quests.missing);
    EXPECT_FALSE(quests.loaded);

    auto slots = saves.listSlots();
    ASSERT_EQ(slots.size(), 2u);
    EXPECT_EQ(slots[0].header.slot, "second"); // newest first
    EXPECT_EQ(slots[0].header.kind, SaveKind::Quick);
    EXPECT_TRUE(slots[1].hasThumbnail);
    EXPECT_EQ(slots[1].header.entityCount, 1u);
    auto thumb = saves.thumbnail("first");
    ASSERT_TRUE(thumb);
    ASSERT_EQ(thumb->size(), 64u);
    EXPECT_EQ((*thumb)[63], std::byte(63));
    EXPECT_TRUE(saves.exists("first"));
    EXPECT_FALSE(saves.save("../evil", w));
    EXPECT_FALSE(SaveGameSystem::isValidSlotName("a.b"));
    EXPECT_TRUE(saves.deleteSlot("first"));
    EXPECT_FALSE(saves.exists("first"));
    EXPECT_FALSE(saves.load("first", w));
}

TEST(SaveGame, MigrationsUpgradeOldSaves) {
    registerTestTypes();
    Fixture f;
    World w;
    Entity e = w.create("Hero");
    e.add<SaveGameComponent>();
    e.add<HealthComponent>().health = 7.0f;
    const Uuid id = e.uuid();
    {
        SaveGameSystem v1(f.config());
        QuestLog quests;
        quests.gold = 250;
        v1.registerSaveable(quests);
        ASSERT_TRUE(v1.save("old", w));
    }
    auto cfg = f.config();
    cfg.version = 3;
    SaveGameSystem v3(cfg);
    QuestLog quests;
    v3.registerSaveable(quests);
    std::vector<u32> applied;
    // v1 -> v2: "gold" in the quest section was renamed to "coins".
    v3.registerMigration(1, [&](serial::Document& doc) -> Status {
        applied.push_back(1);
        serial::Value* q = doc.root.find("sections")->find("quests");
        if (!q) return makeError("no quests");
        const serial::Value* gold = q->find("gold");
        q->set("coins", serial::Value::makeInt(gold ? gold->getInt() : 0, serial::Tag::I32));
        q->erase("gold");
        return {};
    });
    // v2 -> v3: health values were rescaled x10.
    v3.registerMigration(2, [&](serial::Document& doc) -> Status {
        applied.push_back(2);
        for (auto& rec : doc.root.find("world")->find("entities")->items()) {
            if (serial::Value* h = rec.find("components")->find("Test.Health")) {
                if (serial::Value* hp = h->find("health")) *hp = serial::Value::makeF32(f32(hp->getDouble() * 10.0));
            }
        }
        return {};
    });
    World fresh;
    auto r = v3.load("old", fresh);
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(r->migratedFrom, 1u);
    EXPECT_EQ(applied, (std::vector<u32>{1, 2}));
    EXPECT_EQ(quests.coins, 250);
    EXPECT_EQ(quests.gold, 0);
    ASSERT_TRUE(fresh.find(id).valid());
    EXPECT_FLOAT_EQ(fresh.find(id).get<HealthComponent>().health, 70.0f);

    // A failing migration aborts the load.
    v3.registerMigration(1, [](serial::Document&) -> Status { return makeError("cannot upgrade"); });
    auto bad = v3.load("old", fresh);
    ASSERT_FALSE(bad);
    EXPECT_NE(bad.error().message.find("cannot upgrade"), std::string::npos);
}

TEST(SaveGame, CorruptionFallsBackToBackup) {
    registerTestTypes();
    Fixture f;
    SaveGameSystem saves(f.config());
    World w;
    Entity e = w.create("Hero");
    e.add<SaveGameComponent>();
    auto& h = e.add<HealthComponent>();
    h.health = 1.0f;
    ASSERT_TRUE(saves.save("slot", w)); // A
    h.health = 2.0f;
    ASSERT_TRUE(saves.save("slot", w)); // B, A becomes the backup
    const auto path = saves.slotPath("slot");
    auto bak = path;
    bak += ".bak";
    ASSERT_TRUE(std::filesystem::exists(bak));

    // Flip a byte near the end (DATA chunk): CRC mismatch.
    {
        std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
        file.seekg(-8, std::ios::end);
        char c = 0;
        file.read(&c, 1);
        file.seekp(-8, std::ios::end);
        c = char(c ^ 0x5A);
        file.write(&c, 1);
    }
    auto info = saves.slotInfo("slot");
    ASSERT_TRUE(info);
    EXPECT_TRUE(info->corrupted);
    EXPECT_TRUE(info->hasBackup);

    World fresh;
    auto r = saves.load("slot", fresh);
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_TRUE(r->fromBackup);
    EXPECT_FLOAT_EQ(fresh.find(e.uuid()).get<HealthComponent>().health, 1.0f);

    // Truncated file + no backup: clean error.
    ASSERT_TRUE(saves.save("lonely", w));
    std::filesystem::resize_file(saves.slotPath("lonely"), 20);
    EXPECT_FALSE(saves.load("lonely", fresh));
}

TEST(SaveGame, AtomicWriteSurvivesFailureMidWrite) {
    registerTestTypes();
    Fixture f;
    SaveGameSystem saves(f.config());
    World w;
    Entity e = w.create("Hero");
    e.add<SaveGameComponent>();
    auto& h = e.add<HealthComponent>();
    h.health = 1.0f;
    ASSERT_TRUE(saves.save("slot", w));

    // Crash while writing the temp file (only half of the bytes reach the disk).
    h.health = 2.0f;
    saves.setFaultHook([](SaveWriteStage stage, std::span<const std::byte>& bytes) {
        if (stage == SaveWriteStage::WriteTemp) {
            bytes = bytes.first(bytes.size() / 2);
            return false;
        }
        return true;
    });
    EXPECT_FALSE(saves.save("slot", w));
    auto tmp = saves.slotPath("slot");
    tmp += ".tmp";
    EXPECT_FALSE(std::filesystem::exists(tmp));
    World fresh;
    auto r = saves.load("slot", fresh);
    ASSERT_TRUE(r);
    EXPECT_FALSE(r->fromBackup);
    EXPECT_FLOAT_EQ(fresh.find(e.uuid()).get<HealthComponent>().health, 1.0f);

    // Failure when committing the rename: rolled back to the previous save.
    saves.setFaultHook([](SaveWriteStage stage, std::span<const std::byte>&) { return stage != SaveWriteStage::CommitRename; });
    EXPECT_FALSE(saves.save("slot", w));
    World fresh2;
    auto r2 = saves.load("slot", fresh2);
    ASSERT_TRUE(r2);
    EXPECT_FLOAT_EQ(fresh2.find(e.uuid()).get<HealthComponent>().health, 1.0f);

    // Healthy write afterwards.
    saves.setFaultHook({});
    ASSERT_TRUE(saves.save("slot", w));
    World fresh3;
    ASSERT_TRUE(saves.load("slot", fresh3));
    EXPECT_FLOAT_EQ(fresh3.find(e.uuid()).get<HealthComponent>().health, 2.0f);
}

TEST(SaveGame, AsyncSaveCapturesConsistentSnapshot) {
    registerTestTypes();
    Fixture f;
    JobSystem jobs(4);
    SaveGameSystem saves(f.config(), &jobs);
    // Slow down the disk write so the world keeps changing while the job runs.
    saves.setFaultHook([](SaveWriteStage stage, std::span<const std::byte>&) {
        if (stage == SaveWriteStage::WriteTemp) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return true;
    });
    World w;
    std::vector<Uuid> ids;
    for (int i = 0; i < 50; ++i) {
        Entity e = w.create("E" + std::to_string(i));
        e.add<SaveGameComponent>();
        e.add<HealthComponent>().health = f32(i);
        ids.push_back(e.uuid());
    }
    int savedSignals = 0;
    auto c = saves.saved.connect([&](const SaveResult&) { ++savedSignals; });
    SaveHandle handle = saves.saveAsync("async", w);
    // Game thread keeps mutating immediately.
    for (auto [e, hc] : w.view<HealthComponent>().each()) hc.health = -1.0f;
    w.destroyImmediate(w.find(ids[0]));
    w.create("Late").add<SaveGameComponent>();
    EXPECT_FALSE(handle->done());
    handle->wait();
    ASSERT_TRUE(handle->result()) << handle->result().error().message;
    EXPECT_EQ(savedSignals, 0); // signals are delivered on the game thread
    saves.update(0.0);
    EXPECT_EQ(savedSignals, 1);

    // Async load: decoding in the background, applied in update().
    World fresh;
    saves.setWorldProvider([&] { return &fresh; });
    LoadHandle load = saves.loadAsync("async");
    for (int i = 0; i < 1000 && !load->done(); ++i) {
        saves.update(0.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(load->done());
    ASSERT_TRUE(load->result()) << load->result().error().message;
    EXPECT_EQ(fresh.entityCount(), 50u);
    for (int i = 0; i < 50; ++i) {
        Entity e = fresh.find(ids[i]);
        ASSERT_TRUE(e.valid());
        EXPECT_FLOAT_EQ(e.get<HealthComponent>().health, f32(i));
    }
    EXPECT_FALSE(fresh.findByName("Late").valid());

    // Concurrent saves to one slot are serialised; the file stays valid.
    std::vector<SaveHandle> handles;
    for (int i = 0; i < 4; ++i) handles.push_back(saves.saveAsync("async", w));
    saves.waitIdle();
    for (auto& hd : handles) EXPECT_TRUE(hd->result());
    EXPECT_TRUE(saves.slotInfo("async").hasValue());
    EXPECT_FALSE(saves.slotInfo("async")->corrupted);
}

TEST(SaveGame, AutosaveRotationAndInterval) {
    registerTestTypes();
    Fixture f;
    JobSystem jobs(2);
    World w;
    w.create("Hero").add<SaveGameComponent>();
    {
        SaveGameSystem saves(f.config(), &jobs);
        saves.setWorldProvider([&] { return &w; });
        for (int i = 0; i < 5; ++i) {
            saves.autosave();
            saves.waitIdle();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::vector<std::string> names;
        for (const auto& s : saves.listSlots()) {
            names.push_back(s.header.slot);
            EXPECT_EQ(s.header.kind, SaveKind::Auto);
        }
        std::sort(names.begin(), names.end());
        EXPECT_EQ(names, (std::vector<std::string>{"autosave0", "autosave1", "autosave2"}));
        // Writes went 0,1,2,0,1: autosave1 is the newest file.
        EXPECT_GT(std::filesystem::last_write_time(saves.slotPath("autosave1")),
                  std::filesystem::last_write_time(saves.slotPath("autosave0")));
    }
    // A new session continues after the newest autosave (autosave2 is the oldest -> next).
    SaveGameSystem saves(f.config(), &jobs);
    saves.setWorldProvider([&] { return &w; });
    saves.setAutosaveInterval(1.0);
    const auto before = std::filesystem::last_write_time(saves.slotPath("autosave2"));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    saves.update(0.6);
    saves.waitIdle();
    EXPECT_EQ(std::filesystem::last_write_time(saves.slotPath("autosave2")), before);
    saves.update(0.6); // 1.2 s of play time -> autosave
    saves.waitIdle();
    EXPECT_GT(std::filesystem::last_write_time(saves.slotPath("autosave2")), before);
    EXPECT_DOUBLE_EQ(saves.playTime(), 1.2);
    saves.update(5.0, /*playing*/ false); // menus/pause do not count
    EXPECT_DOUBLE_EQ(saves.playTime(), 1.2);
}

TEST(SaveGame, JsonExportMatchesBinary) {
    registerTestTypes();
    Fixture f;
    SaveGameSystem saves(f.config());
    QuestLog quests;
    quests.active = {"A", "B"};
    saves.registerSaveable(quests);
    World w;
    Entity e = w.create("Hero");
    e.add<SaveGameComponent>();
    e.add<HealthComponent>().health = 33.0f;
    ASSERT_TRUE(saves.save("export", w));
    const auto out = f.dir / "export.oxsave.json";
    auto json = saves.exportJson("export", out);
    ASSERT_TRUE(json) << json.error().message;
    EXPECT_NE(json->find("\"oxb1-json\""), std::string::npos);
    EXPECT_NE(json->find("SaveGameHeader"), std::string::npos);
    EXPECT_NE(json->find("Test.Health"), std::string::npos);
    ASSERT_TRUE(std::filesystem::exists(out));
    // Lossless: JSON -> binary is byte-identical to the save (oxdump --to-binary).
    auto binary = serial::jsonToBinary(*json);
    ASSERT_TRUE(binary);
    auto original = serial::readFileBytes(saves.slotPath("export"));
    ASSERT_TRUE(original);
    EXPECT_EQ(*binary, *original);
    // And a JSON save can be loaded directly.
    auto doc = serial::parseJsonString(*json);
    ASSERT_TRUE(doc);
    World fresh;
    auto r = saves.apply(*doc, fresh);
    ASSERT_TRUE(r);
    EXPECT_FLOAT_EQ(fresh.find(e.uuid()).get<HealthComponent>().health, 33.0f);
}

TEST(SaveGame, EngineQuickSaveReloadsLevel) {
    registerTestTypes();
    test::TempDir dir;
    const auto levelPath = dir / "level.oxscene";
    {
        World w;
        Entity hero = w.create("Hero");
        hero.add<SaveGameComponent>();
        hero.add<HealthComponent>();
        w.create("Barrel");
        ASSERT_TRUE(saveScene(w, levelPath));
    }
    EngineConfig cfg;
    cfg.headless = true;
    cfg.workerThreads = 2;
    cfg.userDir = dir / "user";
    cfg.startupScene = levelPath.string();
    Engine engine;
    ASSERT_TRUE(engine.init(cfg));
    Entity hero = engine.world().findByName("Hero");
    ASSERT_TRUE(hero.valid());
    hero.get<HealthComponent>().health = 5.0f;
    engine.world().destroyImmediate(engine.world().findByName("Barrel"));
    ASSERT_TRUE(engine.saves().quickSave());
    ASSERT_TRUE(std::filesystem::exists(dir / "user/saves/quicksave.oxsave"));

    hero.get<HealthComponent>().health = 99.0f;
    World* before = &engine.world();
    auto r = engine.saves().quickLoad(); // reloads the level from disk, then applies
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_NE(&engine.world(), before);
    EXPECT_FLOAT_EQ(engine.world().findByName("Hero").get<HealthComponent>().health, 5.0f);
    EXPECT_FALSE(engine.world().findByName("Barrel").valid());
    EXPECT_EQ(engine.currentLevel(), levelPath.string());
    ASSERT_TRUE(engine.console().execute("save console"));
    EXPECT_TRUE(engine.saves().exists("console"));
}
