// Engine <-> assets module (AssetRegistry in dev builds, cooked .oxpak), gameplay providers, and coroutine owner
// ids on the runtime's world unload / entity destroy paths.
#include "test_util.hpp"

#include <oxwald/runtime/engine.hpp>
#include <oxwald/runtime/launch.hpp>
#include <oxwald/scene/prefab.hpp>
#include <oxwald/scene/runtime_id.hpp>
#include <oxwald/scene/scene.hpp>
#include <oxwald/scene/scene_serializer.hpp>

#if OX_HAS_ASSETS
#include <oxwald/assets/assets.hpp>
#include <oxwald/core/vfs.hpp>
#endif
#if OX_HAS_GAMEPLAY
#include <oxwald/gameplay/gameplay.hpp>
#endif
#if OX_HAS_ASYNC
#include <oxwald/async/scheduler.hpp>
#include <oxwald/async/task.hpp>
#endif

#include <gtest/gtest.h>

#include <fstream>

using namespace ox;

namespace {

EngineConfig baseConfig(const test::TempDir& dir) {
    EngineConfig c;
    c.headless = true;
    c.workerThreads = 2;
    c.userDir = dir / "user";
    c.fixedRate = 60.0;
    c.saveUserSettingsOnShutdown = false;
    return c;
}

void writeText(const std::filesystem::path& p, std::string_view text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

[[maybe_unused]] void writeLevel(const std::filesystem::path& path, std::initializer_list<const char*> names) {
    World w;
    for (const char* n : names) w.create(n);
    std::filesystem::create_directories(path.parent_path());
    ASSERT_TRUE(saveScene(w, path));
}

} // namespace

TEST(RuntimeLaunch, PakOptionReachesTheEngineConfig) {
    auto o = parseLaunchOptions(std::vector<std::string>{"--pak", "Game.oxpak", "--headless"});
    ASSERT_TRUE(o);
    EXPECT_EQ(o->toEngineConfig().pakPath, std::filesystem::path("Game.oxpak"));
}

#if OX_HAS_ASSETS
namespace {

// A small project: <root>/Game.oxproj, Assets/Levels/start.oxscene (+ a prefab and a script when gameplay is in).
std::filesystem::path makeProject(const test::TempDir& dir) {
    registerSceneTypes();
#if OX_HAS_GAMEPLAY
    registerGameplayTypes();
#endif
    const auto root = dir / "Game";
    writeLevel(root / "Assets/Levels/start.oxscene", {"Hero", "Ground", "Sky"});
    writeText(root / "Assets/Scripts/hello.lua", "function onUpdate(self, dt) self.entity.name = 'scripted' end\n");
#if OX_HAS_GAMEPLAY
    World proto;
    Entity crate = proto.create("Crate");
    crate.add<LightComponent>().intensity = 42.f;
    const serial::Document prefab = createPrefab(proto, crate, {.linkSource = false});
    EXPECT_TRUE(serial::saveDocument(root / "Assets/Prefabs/crate.oxprefab", prefab, serial::Format::Binary));
#endif
    Project p = Project::create(root, "Game");
    p.settings.startupScene = "project://Assets/Levels/start.oxscene";
    EXPECT_TRUE(p.save());
    return root;
}

} // namespace

TEST(RuntimeAssets, ProjectAssetDatabaseIsAnEngineService) {
    test::TempDir dir;
    const auto root = makeProject(dir);
    Engine engine;
    auto cfg = baseConfig(dir);
    cfg.projectPath = root;
    ASSERT_TRUE(engine.init(cfg));
    Services& s = engine.services();
    ASSERT_TRUE(s.has<assets::AssetRegistry>()) << "dev/editor builds import from the project";
    ASSERT_TRUE(s.has<assets::AssetManager>());
    EXPECT_EQ(&s.get<assets::IAssetSource>(), static_cast<assets::IAssetSource*>(&s.get<assets::AssetRegistry>()));
    EXPECT_NE(engine.findModule("assets"), nullptr);
    EXPECT_EQ(engine.currentLevel(), "project://Assets/Levels/start.oxscene");
    EXPECT_EQ(engine.world().entityCount(), 3u);

    auto& registry = s.get<assets::AssetRegistry>();
    EXPECT_EQ(registry.assetsDir(), std::filesystem::absolute(root / "Assets").lexically_normal());
    auto scene = s.get<assets::AssetManager>().loadSync<assets::SceneAsset>("Levels/start.oxscene");
    ASSERT_TRUE(scene.isLoaded());
    EXPECT_EQ(scene.get()->document.kind, "scene");
#if OX_HAS_GAMEPLAY && OX_GAMEPLAY_HAS_ASSETS
    ASSERT_TRUE(s.has<gameplay::IPrefabProvider>());
    EXPECT_TRUE(s.has<gameplay::GameplayAssetEvents>());
    auto doc = s.get<gameplay::IPrefabProvider>().prefab("crate");
    ASSERT_TRUE(doc);
    auto script = s.get<gameplay::IScriptSourceProvider>().scriptByName("hello");
    ASSERT_TRUE(script);
    EXPECT_NE(script->source.find("scripted"), std::string::npos);
    // Gameplay scripts resolve through the asset database.
    Entity e = engine.world().create("Scripted");
    e.add<gameplay::ScriptComponent>().script = "Scripts/hello.lua";
    for (int i = 0; i < 3; ++i) engine.tick(1.0 / 60.0);
    EXPECT_EQ(e.name(), "scripted");
#endif
    for (int i = 0; i < 3; ++i) engine.tick(1.0 / 60.0); // AssetManager::update every frame
}

TEST(RuntimeAssets, CookedGameRunsFromThePakAlone) {
    test::TempDir dir;
    const auto root = makeProject(dir);
    const auto pakPath = dir / "Game.oxpak";
    {
        assets::AssetRegistry registry(root);
#if OX_HAS_GAMEPLAY && OX_GAMEPLAY_HAS_ASSETS
        gameplay::registerGameplayImporters(registry.importers());
#endif
        auto report = assets::cookProject(registry, pakPath);
        ASSERT_TRUE(report);
        EXPECT_TRUE(report->errors.empty());
    }
    std::filesystem::remove_all(root); // only the pak ships

    Engine engine;
    auto cfg = baseConfig(dir);
    cfg.pakPath = pakPath;
    ASSERT_TRUE(engine.init(cfg));
    EXPECT_EQ(engine.projectSettings().name, "Game") << "project file read from the pak";
    EXPECT_FALSE(engine.services().has<assets::AssetRegistry>()) << "cooked: no importers";
    ASSERT_TRUE(engine.services().has<assets::PakAssetSource>());
    EXPECT_EQ(engine.currentLevel(), "project://Assets/Levels/start.oxscene");
    EXPECT_EQ(engine.world().entityCount(), 3u) << "startup scene loaded through project:// from the pak";
    EXPECT_TRUE(engine.vfs().exists("project://Game.oxproj"));
    auto listed = engine.vfs().list("project://Assets/Levels", false);
    EXPECT_NE(std::find(listed.begin(), listed.end(), "project://Assets/Levels/start.oxscene"), listed.end());

    // Async level change also reads from the pak.
    engine.requestLevelChange("project://Assets/Levels/start.oxscene");
    for (int i = 0; i < 300 && (engine.loading() || engine.world().entityCount() != 3u); ++i) engine.tick(1.0 / 60.0);
    EXPECT_FALSE(engine.loading());
#if OX_HAS_GAMEPLAY && OX_GAMEPLAY_HAS_ASSETS
    auto doc = engine.services().get<gameplay::IPrefabProvider>().prefab("Prefabs/crate");
    ASSERT_TRUE(doc);
    auto inst = instantiatePrefab(engine.world(), *doc);
    ASSERT_TRUE(inst);
    EXPECT_FLOAT_EQ(inst->get<LightComponent>().intensity, 42.f);
#endif
}

TEST(RuntimeAssets, MissingPakFailsInit) {
    test::TempDir dir;
    Engine engine;
    auto cfg = baseConfig(dir);
    cfg.pakPath = dir / "missing.oxpak";
    EXPECT_FALSE(engine.init(cfg));
}
#endif

#if OX_HAS_ASYNC
namespace {

struct Flag {
    bool* destroyed;
    ~Flag() { *destroyed = true; }
};

Task<> waitForever(bool* destroyed) {
    Flag f{destroyed};
    co_await seconds(1e9);
}

} // namespace

// Gameplay (when present) and the runtime cancel entity coroutines with the same owner id.
TEST(RuntimeCoroutines, EntityOwnerIdsMatchOnDestroyAndUnloadPaths) {
    for (const bool withGameplay : {true, false}) {
        SCOPED_TRACE(withGameplay ? "gameplay module" : "runtime only");
        test::TempDir dir;
        registerSceneTypes();
        const auto root = dir / "Game";
        writeLevel(root / "levels/extra.oxscene", {"Spawned"});
        Project p = Project::create(root, "Game");
        if (!withGameplay) p.settings.modules["gameplay"] = false;
        ASSERT_TRUE(p.save());

        Engine engine;
        auto cfg = baseConfig(dir);
        cfg.projectPath = root;
        cfg.editor = true;
        ASSERT_TRUE(engine.init(cfg));
#if !OX_HAS_GAMEPLAY
        if (withGameplay) continue;
#endif
        auto& sched = engine.services().get<CoroutineScheduler>();
        engine.enterPlayMode();

        // Additive scene unloaded -> World::destroyImmediate path.
        auto roots = engine.loadSceneAdditive("project://levels/extra.oxscene");
        ASSERT_TRUE(roots && roots->size() == 1);
        const Entity spawned = roots->front();
#if OX_HAS_GAMEPLAY
        EXPECT_EQ(gameplay::coroutineOwner(spawned), entityRuntimeId(spawned.handle()));
#endif
        bool spawnedDone = false;
        sched.spawn(waitForever(&spawnedDone), {.name = "spawned", .owner = entityRuntimeId(spawned.handle())});

        // Entity::destroy (deferred) path.
        Entity doomed = engine.world().create("Doomed");
        bool doomedDone = false;
        sched.spawn(waitForever(&doomedDone), {.name = "doomed", .owner = entityRuntimeId(doomed.handle())});

        // Survivors: an entity coroutine of a living entity and a global (owner-less) one.
        Entity alive = engine.world().create("Alive");
        bool aliveDone = false, globalDone = false;
        sched.spawn(waitForever(&aliveDone), {.name = "alive", .owner = entityRuntimeId(alive.handle())});
        sched.spawn(waitForever(&globalDone), {.name = "global"});
        engine.tick(1.0 / 60.0);

        ASSERT_TRUE(engine.unloadAdditive("project://levels/extra.oxscene"));
        EXPECT_TRUE(spawnedDone) << "cancelled when its entity was destroyed";
        doomed.destroy();
        EXPECT_TRUE(doomedDone) << "cancelled as soon as the entity is scheduled for destruction";
        engine.tick(1.0 / 60.0);
        EXPECT_FALSE(aliveDone);
        EXPECT_FALSE(globalDone);

        // Leaving play mode drops the play world's entity coroutines but not global ones (editor tools).
        engine.exitPlayMode();
        EXPECT_TRUE(aliveDone);
        EXPECT_FALSE(globalDone);
        // A level change cancels everything.
        engine.setWorld(std::make_unique<World>(), "test://other");
        EXPECT_TRUE(globalDone);
        EXPECT_EQ(sched.liveCount(), 0u);
    }
}
#endif
