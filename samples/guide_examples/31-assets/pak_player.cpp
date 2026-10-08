// Глава 31: проект → .oxpak → запуск движка только из пака, как `OxwaldPlayer --pak Game.oxpak`
// (docs/guide/31-assets.md).
#include "project_fixture.hpp"

#include <oxwald/assets/assets.hpp>
#include <oxwald/runtime/engine.hpp>
#include <oxwald/runtime/launch.hpp>
#include <oxwald/runtime/project.hpp>
#include <oxwald/scene/scene.hpp>
#include <oxwald/scene/scene_serializer.hpp>

#include <gtest/gtest.h>

using namespace ox;
using namespace guide31;

TEST(GuideAssetsPlayer, RunCookedGameFromPak) {
    TempProject temp;
    const auto root = temp.root() / "MyGame";

    // 1. Проект: MyGame.oxproj + Assets/Levels/start.oxscene.
    registerSceneTypes();
    {
        World level;
        level.create("Player");
        level.create("Sun").add<LightComponent>().type = LightType::Directional;
        std::filesystem::create_directories(root / "Assets/Levels");
        ASSERT_TRUE(saveScene(level, root / "Assets/Levels/start.oxscene"));
    }
    Project project = Project::create(root, "MyGame");
    project.settings.startupScene = "project://Assets/Levels/start.oxscene";
    ASSERT_TRUE(project.save());

    // 2. Сборка (= oxpack MyGame -o Game.oxpak --scene Levels/start.oxscene).
    const auto pak = temp.root() / "Game.oxpak";
    {
        assets::AssetRegistry registry(root);
        registry.scan();
        auto report = assets::cookProject(registry, pak, {.startupScenes = {"Levels/start.oxscene"}});
        ASSERT_TRUE(report) << report.error().message;
        EXPECT_TRUE(report->errors.empty());
    }
    std::filesystem::remove_all(root);   // .oxproj едет внутри пака; папка проекта больше не нужна

    // 3. Запуск: OxwaldPlayer --pak Game.oxpak --headless --user-dir <dir>.
    const std::vector<std::string> args{"--pak", pak.string(), "--headless", "--user-dir", (temp.root() / "user").string()};
    auto options = parseLaunchOptions(args);
    ASSERT_TRUE(options) << options.error().message;
    EngineConfig config = options->toEngineConfig("OxwaldPlayer");
    EXPECT_EQ(config.pakPath, pak);
    config.saveUserSettingsOnShutdown = false;

    Engine engine;
    ASSERT_TRUE(engine.init(config));
    EXPECT_EQ(engine.projectSettings().name, "MyGame");   // настройки проекта прочитаны из пака
    EXPECT_FALSE(engine.services().has<assets::AssetRegistry>());   // в собранной игре импортёров нет
    EXPECT_TRUE(engine.services().has<assets::PakAssetSource>());
    EXPECT_EQ(engine.currentLevel(), "project://Assets/Levels/start.oxscene");
    EXPECT_TRUE(engine.world().findByName("Sun").valid());
    for (int i = 0; i < 3; ++i) engine.tick(1.0 / 60.0);
    engine.shutdown();
}
