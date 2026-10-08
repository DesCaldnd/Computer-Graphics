// Глава 05: проект .oxproj, стартовая сцена, additive-сцены, асинхронная смена уровня (docs/guide/05-runtime.md).
#include <oxwald/runtime/engine.hpp>
#include <oxwald/runtime/json_io.hpp>
#include <oxwald/runtime/project.hpp>
#include <oxwald/scene/scene.hpp>
#include <oxwald/scene/scene_serializer.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

void writeLevel(const std::filesystem::path& path, std::initializer_list<const char*> names) {
    ox::World world;
    for (const char* n : names) world.create(n);
    ASSERT_TRUE(ox::saveScene(world, path));
}

struct TempDir {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("oxwald_guide_levels_" + ox::Uuid::generate().toString());
    TempDir() { std::filesystem::create_directories(path); }
    ~TempDir() { std::filesystem::remove_all(path); }
};

} // namespace

TEST(GuideRuntimeProject, CreateProjectFile) {
    TempDir tmp;
    // Создаём MyGame/MyGame.oxproj — обычный JSON, его можно править руками.
    ox::Project project = ox::Project::create(tmp.path / "MyGame", "MyGame");
    project.settings.version = "1.0.0";
    project.settings.startupScene = "project://levels/main.oxscene";
    project.settings.physics.fixedRate = 50.0f;
    project.settings.modules["audio"] = false; // встроенный модуль можно выключить
    std::filesystem::create_directories(tmp.path / "MyGame");
    ASSERT_TRUE(project.save());

    auto json = ox::json::loadFile(tmp.path / "MyGame" / "MyGame.oxproj");
    ASSERT_TRUE(json);
    EXPECT_EQ((*json)["startupScene"], "project://levels/main.oxscene");
    EXPECT_TRUE((*json)["physics"]["gravity"].is_array()); // vec3 -> [x, y, z]
    EXPECT_EQ((*json)["physics"]["fixedRate"], 50.0);
    EXPECT_EQ((*json)["modules"]["audio"], false);

    auto loaded = ox::Project::load(tmp.path / "MyGame"); // каталог или путь к .oxproj
    ASSERT_TRUE(loaded) << loaded.error().message;
    EXPECT_EQ(loaded->settings.physics.fixedRate, 50.0f);
    EXPECT_FALSE(loaded->settings.moduleEnabled("audio"));
    EXPECT_TRUE(loaded->settings.moduleEnabled("physics")); // нет в списке = включён
}

TEST(GuideRuntimeProject, LevelsAdditiveAndAsyncChange) {
    TempDir tmp;
    ox::registerSceneTypes();
    const auto root = tmp.path / "Game";
    std::filesystem::create_directories(root / "levels");
    writeLevel(root / "levels" / "main.oxscene", {"Hero", "Ground"});
    writeLevel(root / "levels" / "props.oxscene", {"Tree", "Rock"});
    writeLevel(root / "levels" / "boss.oxscene", {"Boss"});
    ox::Project project = ox::Project::create(root, "Game");
    project.settings.startupScene = "project://levels/main.oxscene";
    ASSERT_TRUE(project.save());

    ox::EngineConfig config;
    config.headless = true;
    config.workerThreads = 2;
    config.projectPath = root;        // project:// = каталог проекта
    config.userDir = tmp.path / "user"; // user:// = настройки и сохранения
    ox::Engine engine;
    ASSERT_TRUE(engine.init(config));
    EXPECT_EQ(engine.currentLevel(), "project://levels/main.oxscene"); // стартовая сцена загружена
    EXPECT_EQ(engine.world().entityCount(), 2u);

    // Additive: догружаем сцену в текущий мир и выгружаем обратно.
    auto added = engine.loadSceneAdditive("project://levels/props.oxscene");
    ASSERT_TRUE(added);
    EXPECT_EQ(engine.world().entityCount(), 4u);
    ASSERT_TRUE(engine.unloadAdditive("project://levels/props.oxscene"));
    EXPECT_EQ(engine.world().entityCount(), 2u);

    // Асинхронная смена уровня: чтение и декодирование в фоне, подмена мира в начале одного из следующих кадров.
    std::vector<std::string> log;
    engine.setLoadingScreenHooks({
        .begin = [&](const std::string& level) { log.push_back("show loading: " + level); },
        .end = [&](const std::string& level, bool ok) { log.push_back((ok ? "hide loading: " : "failed: ") + level); },
    });
    ox::ScopedConnection onLoaded = engine.levelLoaded.connect([&](const std::string& level) { log.push_back("loaded: " + level); });

    engine.requestLevelChange("project://levels/boss.oxscene");
    for (int i = 0; i < 1000 && (log.empty() || engine.loading()); ++i) {
        engine.tick(1.0 / 60.0); // игра продолжает крутиться: FrameContext::loading = true
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_FALSE(engine.loading());
    EXPECT_EQ(engine.currentLevel(), "project://levels/boss.oxscene");
    EXPECT_TRUE(engine.world().findByName("Boss").valid());
    EXPECT_EQ(log.front(), "show loading: project://levels/boss.oxscene");
    EXPECT_EQ(log.back(), "hide loading: project://levels/boss.oxscene");

    // Синхронная загрузка (блокирует кадр): удобно в тестах и инструментах.
    ASSERT_TRUE(engine.loadScene("project://levels/main.oxscene"));
    EXPECT_TRUE(engine.world().findByName("Hero").valid());
    engine.shutdown();
}
