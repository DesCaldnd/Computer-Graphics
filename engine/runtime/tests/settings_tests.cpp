#include "test_util.hpp"

#include <oxwald/core/scalability.hpp>
#include <oxwald/core/vfs.hpp>
#include <oxwald/runtime/console.hpp>
#include <oxwald/runtime/json_io.hpp>
#include <oxwald/runtime/launch.hpp>
#include <oxwald/runtime/project.hpp>
#include <oxwald/runtime/settings.hpp>

#include <gtest/gtest.h>

#include <iterator>

using namespace ox;

namespace {
// Scalability-bound test cvar (renderer cvars do not exist in this test binary).
CVar<int> cvTestShadowRes("test.Shadows.Resolution", 2048, "test", Scalability::Shadows, {512, 1024, 2048, 4096});
CVar<int> cvTestTextures("test.Textures.Pool", 1000, "test", Scalability::Textures, {250, 500, 1000, 2000});
CVar<float> cvTestPersist("test.Persisted", 1.0f, "test", CVarFlags::Persist);
} // namespace

TEST(Project, SaveLoadRoundTrip) {
    test::TempDir dir;
    Project p = Project::create(dir.path(), "Showcase");
    auto& s = p.settings;
    s.version = "1.2.3";
    s.company = "Oxwald";
    s.startupScene = "project://levels/intro.oxscene";
    s.assetDirs = {"assets", "content"};
    s.modules = {{"physics", true}, {"net", false}};
    s.saveVersion = 4;
    s.physics.gravity = {0, -3.7f, 0};
    s.physics.fixedRate = 120.0f;
    s.audio.busVolumes = {{"Music", 0.5f}};
    s.rendering.cvars = {{"r.Bloom", "0"}};
    s.defaultQuality = "Medium";
    s.scalability = {{"Shadows", "Low"}};
    s.packaging.alwaysIncludeAssets = {"project://ui/**"};
    s.packaging.targetPlatforms = {"macos"};
    s.input.actions = {{"Jump", InputValueType::Bool}};
    s.input.contexts = {{"Default", 1, {InputBinding{"Jump", "Key.Space", {}, {InputTrigger::pressed()}, true}}}};
    s.input.activeContexts = {"Default"};
    ASSERT_TRUE(p.save());
    ASSERT_TRUE(std::filesystem::exists(dir / "Showcase.oxproj"));

    // Plain, editable JSON.
    auto text = json::loadFile(dir / "Showcase.oxproj");
    ASSERT_TRUE(text);
    EXPECT_EQ((*text)["name"], "Showcase");
    EXPECT_EQ((*text)["physics"]["fixedRate"], 120.0);

    auto loaded = Project::load(dir.path()); // directory form
    ASSERT_TRUE(loaded) << loaded.error().message;
    const auto& l = loaded->settings;
    EXPECT_EQ(loaded->root(), std::filesystem::absolute(dir.path()));
    EXPECT_EQ(l.name, "Showcase");
    EXPECT_EQ(l.version, "1.2.3");
    EXPECT_EQ(l.company, "Oxwald");
    EXPECT_EQ(l.startupScene, s.startupScene);
    EXPECT_EQ(l.assetDirs, s.assetDirs);
    EXPECT_TRUE(l.moduleEnabled("physics"));
    EXPECT_FALSE(l.moduleEnabled("net"));
    EXPECT_TRUE(l.moduleEnabled("audio"));
    EXPECT_EQ(l.saveVersion, 4u);
    EXPECT_EQ(l.physics.gravity, glm::vec3(0, -3.7f, 0));
    EXPECT_EQ(l.physics.fixedRate, 120.0f);
    EXPECT_EQ(l.audio.busVolumes.at("Music"), 0.5f);
    EXPECT_EQ(l.rendering.cvars.at("r.Bloom"), "0");
    EXPECT_EQ(l.defaultQuality, "Medium");
    EXPECT_EQ(l.scalability.at("Shadows"), "Low");
    EXPECT_EQ(l.packaging.alwaysIncludeAssets, s.packaging.alwaysIncludeAssets);
    EXPECT_EQ(l.packaging.targetPlatforms, s.packaging.targetPlatforms);
    ASSERT_EQ(l.input.contexts.size(), 1u);
    EXPECT_EQ(l.input.contexts[0].bindings[0].source, "Key.Space");
    EXPECT_EQ(l.input.contexts[0].bindings[0].triggers[0].type, InputTriggerType::Pressed);

    // Hand-written minimal project: missing fields keep defaults.
    const auto minimal = dir / "min" / "Min.oxproj";
    std::filesystem::create_directories(minimal.parent_path());
    ASSERT_TRUE(json::saveFile(minimal, nlohmann::ordered_json{{"name", "Min"}, {"unknownField", 5}}));
    auto m = Project::load(minimal);
    ASSERT_TRUE(m);
    EXPECT_EQ(m->settings.name, "Min");
    EXPECT_EQ(m->settings.physics.fixedRate, 60.0f);
    EXPECT_FALSE(Project::load(dir / "nothing_here"));
}

TEST(Settings, ApplyToCVarsAndScalability) {
    test::TempDir dir;
    Vfs vfs;
    vfs.mount("user", std::make_unique<DirectoryMount>(dir.path(), true));
    Settings settings(&vfs);

    ProjectSettings project;
    project.defaultQuality = "Low";
    project.scalability = {{"Textures", "Ultra"}};
    settings.setProject(project);
    settings.applyProjectDefaults();
    EXPECT_EQ(scalability::currentLevel(Scalability::Shadows), QualityLevel::Low);
    EXPECT_EQ(cvTestShadowRes.get(), 512);
    EXPECT_EQ(cvTestTextures.get(), 2000);

    std::vector<SettingsCategory> notified;
    auto c = settings.changed.connect([&](SettingsCategory cat) { notified.push_back(cat); });

    GraphicsSettings g;
    g.quality = "High";
    g.groups = {{"Shadows", "Ultra"}};
    g.vsync = false;
    g.maxFps = 144;
    g.windowMode = WindowMode::Borderless;
    g.resolution = {2560, 1440};
    g.fov = 100.0f;
    g.upscaler = "FSR1";
    g.upscalerQuality = "Balanced";
    g.rayTracing = true;
    settings.setGraphics(g);
    EXPECT_EQ(notified, std::vector<SettingsCategory>{SettingsCategory::Graphics});
    EXPECT_EQ(cvTestTextures.get(), 1000);  // High
    EXPECT_EQ(cvTestShadowRes.get(), 4096); // group override Ultra
    EXPECT_FALSE(cvars::vsync().get());
    EXPECT_EQ(cvars::maxFps().get(), 144);
    EXPECT_EQ(cvars::windowMode().get(), int(WindowMode::Borderless));
    EXPECT_EQ(cvars::resolutionX().get(), 2560);
    EXPECT_FLOAT_EQ(cvars::fov().get(), 100.0f);

    // Renderer cvars registered later pick up the pending values.
    {
        CVar<int> upscaler("r.Upscaler", 0, "Upscaler", CVarEnum{"Off", "FSR1", "DLSS"});
        CVar<bool> rt("r.RayTracing", false, "Ray tracing");
        EXPECT_EQ(upscaler.get(), 1);
        EXPECT_TRUE(rt.get());
    }

    AudioSettings a;
    a.masterVolume = 0.25f;
    a.busVolumes = {{"Music", 0.5f}};
    settings.setAudio(a);
    EXPECT_FLOAT_EQ(cvars::masterVolume().get(), 0.25f);
    EXPECT_FLOAT_EQ(settings.busVolume("Music"), 0.5f);
    EXPECT_FLOAT_EQ(settings.busVolume("SFX"), 1.0f);
    EXPECT_EQ(notified.back(), SettingsCategory::Audio);
}

TEST(Settings, SaveLoadAndCaptureConsoleChanges) {
    test::TempDir dir;
    Vfs vfs;
    vfs.mount("user", std::make_unique<DirectoryMount>(dir.path(), true));
    {
        Settings settings(&vfs);
        ASSERT_TRUE(settings.load()); // missing file = defaults
        scalability::setOverall(QualityLevel::Medium);
        ASSERT_TRUE(CVarRegistry::instance().execute("r.VSync false"));
        ASSERT_TRUE(CVarRegistry::instance().execute("test.Persisted 3.5"));
        ASSERT_TRUE(CVarRegistry::instance().execute("test.Shadows.Resolution 777")); // Shadows -> Custom
        settings.user().language = "ru";
        settings.user().inputRebinds = {{"Default/Jump/0", "Key.K"}};
        settings.captureFromCVars();
        EXPECT_EQ(settings.user().graphics.quality, "Custom");
        EXPECT_EQ(settings.user().graphics.groups.at("Shadows"), "Medium");
        EXPECT_FALSE(settings.user().graphics.vsync);
        ASSERT_TRUE(settings.save());
    }
    ASSERT_TRUE(std::filesystem::exists(dir / "settings.json"));
    // Reset everything, then load + apply.
    cvars::vsync().reset();
    cvTestPersist.reset();
    scalability::setOverall(QualityLevel::Ultra);
    Settings settings(&vfs);
    ASSERT_TRUE(settings.load());
    EXPECT_EQ(settings.user().language, "ru");
    EXPECT_EQ(settings.user().inputRebinds.at("Default/Jump/0"), "Key.K");
    settings.apply();
    EXPECT_FALSE(cvars::vsync().get());
    EXPECT_FLOAT_EQ(cvTestPersist.get(), 3.5f);
    EXPECT_EQ(scalability::currentLevel(Scalability::Textures), QualityLevel::Medium);
    EXPECT_EQ(cvTestShadowRes.get(), 777);
}

TEST(Console, ExecuteCompleteAndHistory) {
    Console console;
    console.addCommand("echo", "Echo args", [](std::span<const std::string> args) {
        std::string out;
        for (const auto& a : args) out += (out.empty() ? "" : " ") + a;
        return out;
    });
    auto r = console.execute("echo hello \"big world\"");
    ASSERT_TRUE(r);
    EXPECT_EQ(*r, "hello big world");
    std::string changed;
    auto c = console.cvarChanged.connect([&](const std::string& n) { changed = n; });
    ASSERT_TRUE(console.execute("r.VSync false; t.MaxFPS 30"));
    EXPECT_FALSE(cvars::vsync().get());
    EXPECT_EQ(cvars::maxFps().get(), 30);
    EXPECT_EQ(changed, "t.MaxFPS");
    auto q = console.execute("t.MaxFPS");
    ASSERT_TRUE(q);
    EXPECT_NE(q->find("30"), std::string::npos);
    EXPECT_FALSE(console.execute("no.such.thing 1"));

    // Completion: names, then enum/bool values.
    auto names = console.complete("r.Vs");
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "r.VSync");
    EXPECT_EQ(console.complete("r.WindowMode B"), std::vector<std::string>{"r.WindowMode Borderless"});
    EXPECT_EQ(console.complete("r.VSync t"), std::vector<std::string>{"r.VSync true"});
    EXPECT_EQ(console.completeCommonPrefix("ec"), "echo");
    EXPECT_FALSE(console.complete("he").empty()); // built-in help

    // History navigation.
    EXPECT_EQ(console.history().size(), 4u);
    EXPECT_EQ(console.historyPrev(), "no.such.thing 1");
    EXPECT_EQ(console.historyPrev(), "t.MaxFPS");
    EXPECT_EQ(console.historyNext(), "no.such.thing 1");
    EXPECT_EQ(console.historyNext(), "");
    const std::string saved = console.saveHistory();
    Console other;
    other.loadHistory(saved);
    EXPECT_EQ(other.history(), console.history());

    // Output buffer + log capture.
    console.captureLog(log::Level::Warn);
    OX_LOG_WARN("test", "captured {}", 42);
    OX_LOG_INFO("test", "not captured");
    bool found = false;
    for (const auto& line : console.output()) found = found || line.text.find("captured 42") != std::string::npos;
    EXPECT_TRUE(found);
    console.stopCaptureLog();
    ASSERT_TRUE(console.execute("clear"));
    // "clear" clears and the input line itself is printed before execution.
    EXPECT_LE(console.output().size(), 1u);
}

TEST(Launch, ParseCommandLine) {
    const char* argv[] = {"OxwaldPlayer", "--project", "/games/demo", "--scene", "project://a.oxscene", "--headless",
                          "--frames",     "10",        "--width",     "1280",    "--height",            "720",
                          "--fullscreen", "--quality", "ultra",       "--cvar",  "r.VSync=false",       "--cvar",
                          "t.MaxFPS=60",  "--single-thread", "--fixed-rate", "120"};
    auto o = parseLaunchOptions(int(std::size(argv)), argv);
    ASSERT_TRUE(o) << o.error().message;
    EXPECT_EQ(o->project, "/games/demo");
    EXPECT_EQ(o->scene, "project://a.oxscene");
    EXPECT_TRUE(o->headless);
    EXPECT_FALSE(o->server);
    EXPECT_EQ(o->frames, 10u);
    EXPECT_EQ(o->width, 1280);
    EXPECT_EQ(o->height, 720);
    EXPECT_EQ(o->windowMode, WindowMode::Fullscreen);
    EXPECT_EQ(o->quality, QualityLevel::Ultra);
    EXPECT_EQ(o->cvars.size(), 2u);
    EXPECT_TRUE(o->singleThread);
    const EngineConfig cfg = o->toEngineConfig();
    EXPECT_TRUE(cfg.headless);
    EXPECT_FALSE(cfg.threadedRendering);
    EXPECT_EQ(cfg.fixedRate, 120.0);
    EXPECT_NE(std::find(cfg.cvars.begin(), cfg.cvars.end(), "r.WindowMode=2"), cfg.cvars.end());
    EXPECT_NE(std::find(cfg.cvars.begin(), cfg.cvars.end(), "r.ResolutionX=1280"), cfg.cvars.end());

    const std::vector<std::string> server = {"--server"};
    auto s = parseLaunchOptions(server);
    ASSERT_TRUE(s);
    EXPECT_TRUE(s->server && s->headless);
    EXPECT_TRUE(s->toEngineConfig().dedicatedServer);

    EXPECT_FALSE(parseLaunchOptions(std::vector<std::string>{"--frames"}));
    EXPECT_FALSE(parseLaunchOptions(std::vector<std::string>{"--frames", "-3"}));
    EXPECT_FALSE(parseLaunchOptions(std::vector<std::string>{"--quality", "epic"}));
    EXPECT_FALSE(parseLaunchOptions(std::vector<std::string>{"--cvar", "novalue"}));
    EXPECT_FALSE(parseLaunchOptions(std::vector<std::string>{"--bogus"}));
}

TEST(Project, UnknownModuleTogglesAreReported) {
    ProjectSettings s;
    s.modules = {{"physics", true}, {"phyiscs", false}, {"net", false}, {"mygame", true}};
    EXPECT_EQ(s.unknownModules(), (std::vector<std::string>{"mygame", "phyiscs"}));
    const std::vector<std::string> registered = {"mygame"};
    EXPECT_EQ(s.unknownModules(registered), std::vector<std::string>{"phyiscs"});
    EXPECT_FALSE(s.moduleEnabled("net"));
}

TEST(Settings, FirstLaunchStartsFromProjectDefaults) {
    test::TempDir dir;
    Vfs vfs;
    vfs.mount("user", std::make_unique<DirectoryMount>(dir.path(), true));
    ProjectSettings project;
    project.defaultQuality = "Low";
    project.rendering.rayTracingIfSupported = true;
    project.rendering.upscaler = "FSR1";
    {
        Settings settings(&vfs);
        settings.setProject(project);
        settings.applyProjectDefaults();
        ASSERT_TRUE(settings.load());
        EXPECT_FALSE(settings.hasUserFile());
        settings.seedUserFromProject();
        EXPECT_EQ(settings.user().graphics.quality, "Low");
        EXPECT_TRUE(settings.user().graphics.rayTracing) << "rendering.rayTracingIfSupported";
        EXPECT_EQ(settings.user().graphics.upscaler, "FSR1");
        settings.user().graphics.quality = "Ultra"; // the player changes it
        settings.user().graphics.rayTracing = false;
        ASSERT_TRUE(settings.save());
    }
    Settings again(&vfs);
    again.setProject(project);
    again.applyProjectDefaults();
    ASSERT_TRUE(again.load());
    EXPECT_TRUE(again.hasUserFile()) << "second launch: the saved choice wins, no re-seeding/auto-detect";
    EXPECT_EQ(again.user().graphics.quality, "Ultra");
    EXPECT_FALSE(again.user().graphics.rayTracing);
    scalability::setOverall(QualityLevel::High);
}

TEST(Settings, UpscalerQualityAcceptsDlaaAlias) {
    CVar<int> q("test.UpscalerQualityAlias", 3, "quality",
                CVarEnum{"UltraPerformance", "Performance", "Balanced", "Quality", "Native"}.alias("DLAA", 4));
    ASSERT_TRUE(q.setFromString("DLAA", CVarSource::Config));
    EXPECT_EQ(q.get(), 4);
    EXPECT_EQ(q.toString(), "Native");
    ASSERT_TRUE(q.setFromJson("dlaa", CVarSource::Config));
    EXPECT_EQ(q.get(), 4);
}
