#include <oxwald/core/cvar.hpp>
#include <oxwald/core/scalability.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

using namespace ox;

namespace {
struct CVars : ::testing::Test {
    void SetUp() override {
        CVarRegistry::instance().setCheatsEnabled(false);
        CVarRegistry::instance().clearRestartRequired();
    }
};
} // namespace

TEST_F(CVars, BasicTypesAndParsing) {
    CVar<bool> b("test.Bool", true, "a bool");
    CVar<int> i("test.Int", 5, "an int", 0, 10);
    CVar<float> f("test.Float", 0.5f, "a float");
    CVar<std::string> s("test.String", "hi", "a string");

    EXPECT_TRUE(b.get());
    EXPECT_TRUE(static_cast<bool>(b));
    EXPECT_TRUE(b.setFromString("off", CVarSource::Console));
    EXPECT_FALSE(b.get());
    EXPECT_FALSE(b.setFromString("maybe", CVarSource::Console));

    EXPECT_TRUE(i.setFromString("7", CVarSource::Console));
    EXPECT_EQ(i.get(), 7);
    i.set(100);
    EXPECT_EQ(i.get(), 10) << "clamped to range";
    EXPECT_FALSE(i.setFromString("abc", CVarSource::Console));

    EXPECT_TRUE(f.setFromString("0.1", CVarSource::Console));
    EXPECT_EQ(f.get(), 0.1f);
    EXPECT_EQ(f.toString(), "0.1");
    EXPECT_TRUE(s.setFromString("hello world", CVarSource::Console));
    EXPECT_EQ(s.get(), "hello world");

    EXPECT_FALSE(s.isDefault());
    s.reset();
    EXPECT_TRUE(s.isDefault());
    EXPECT_EQ(CVarRegistry::instance().find("TEST.INT"), &i) << "lookup is case-insensitive";
    EXPECT_EQ(CVarRegistry::instance().findAs<int>("test.Int"), &i);
    EXPECT_EQ(CVarRegistry::instance().findAs<float>("test.Int"), nullptr);
}

TEST_F(CVars, UnregistersOnDestruction) {
    { CVar<int> tmp("test.Temp", 1, "temp"); EXPECT_NE(CVarRegistry::instance().find("test.Temp"), nullptr); }
    EXPECT_EQ(CVarRegistry::instance().find("test.Temp"), nullptr);
}

TEST_F(CVars, EnumAsInt) {
    CVar<int> up("test.Upscaler", 0, "upscaler", CVarEnum{"Off", "FSR1", "DLSS"});
    EXPECT_EQ(up.toString(), "Off");
    EXPECT_TRUE(up.setFromString("dlss", CVarSource::Console));
    EXPECT_EQ(up.get(), 2);
    EXPECT_EQ(up.toString(), "DLSS");
    EXPECT_TRUE(up.setFromString("1", CVarSource::Console));
    EXPECT_EQ(up.toString(), "FSR1");
    up.set(9);
    EXPECT_EQ(up.get(), 2) << "clamped to the last enum entry";
}

TEST_F(CVars, FlagsAndCallbacks) {
    CVar<int> ro("test.ReadOnly", 1, "ro", CVarFlags::ReadOnly);
    CVar<bool> cheat("test.Cheat", false, "cheat", CVarFlags::Cheat);
    CVar<int> restart("test.Restart", 1, "restart", CVarFlags::RequiresRestart);

    EXPECT_FALSE(ro.setFromString("2", CVarSource::Console));
    EXPECT_FALSE(ro.setFromString("2", CVarSource::Config));
    EXPECT_TRUE(ro.set(2));
    EXPECT_EQ(ro.get(), 2);

    EXPECT_FALSE(cheat.setFromString("1", CVarSource::Console));
    EXPECT_TRUE(cheat.setFromString("1", CVarSource::Config));
    CVarRegistry::instance().setCheatsEnabled(true);
    EXPECT_TRUE(cheat.setFromString("0", CVarSource::Console));

    EXPECT_FALSE(CVarRegistry::instance().restartRequired());
    restart.set(2);
    EXPECT_TRUE(CVarRegistry::instance().restartRequired());

    int calls = 0;
    int lastOld = -1;
    const auto id = restart.onChanged([&](int now, int old) {
        ++calls;
        lastOld = old;
        EXPECT_EQ(now, 3);
    });
    restart.set(3);
    restart.set(3); // unchanged: no callback
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(lastOld, 2);
    restart.removeCallback(id);
    restart.set(4);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(restart.lastSource(), CVarSource::Code);
}

TEST_F(CVars, ConsoleExecute) {
    CVar<float> gamma("test.Gamma", 2.2f, "gamma");
    CVar<std::string> name("test.Name", "", "name");
    std::vector<std::string> received;
    ConsoleCommand cmd("test.Echo", "echo args", [&](std::span<const std::string> args) {
        received.assign(args.begin(), args.end());
        return std::string("echoed");
    });
    auto& reg = CVarRegistry::instance();
    auto r = reg.execute("test.Gamma 1.8");
    ASSERT_TRUE(r);
    EXPECT_EQ(gamma.get(), 1.8f);
    auto show = reg.execute("test.Gamma");
    ASSERT_TRUE(show);
    EXPECT_NE(show->find("1.8"), std::string::npos);
    EXPECT_TRUE(reg.execute("test.Name \"Big Bob\""));
    EXPECT_EQ(name.get(), "Big Bob");
    auto out = reg.execute(R"(test.Echo a "b c" d\ e)");
    ASSERT_TRUE(out);
    EXPECT_EQ(*out, "echoed");
    EXPECT_EQ(received, (std::vector<std::string>{"a", "b c", "d e"}));
    EXPECT_FALSE(reg.execute("test.Gamma notanumber"));
    EXPECT_FALSE(reg.execute("does.not.exist 1"));
    auto completions = reg.complete("test.G");
    EXPECT_NE(std::find(completions.begin(), completions.end(), "test.Gamma"), completions.end());
}

TEST_F(CVars, SaveAndLoadOverrides) {
    auto& reg = CVarRegistry::instance();
    nlohmann::json saved;
    {
        CVar<int> persisted("test.Persisted", 1, "p", CVarFlags::Persist);
        CVar<int> transient("test.Transient", 1, "t");
        CVar<std::string> str("test.PersistStr", "a", "s", CVarFlags::Persist);
        persisted.set(5);
        transient.set(6);
        str.set("b");
        saved = reg.saveOverrides();
        EXPECT_EQ(saved["test.Persisted"], 5);
        EXPECT_EQ(saved["test.PersistStr"], "b");
        EXPECT_FALSE(saved.contains("test.Transient"));
        EXPECT_TRUE(reg.saveOverrides(true).contains("test.Transient"));
    }
    // Loading before the cvars exist keeps values pending until they register.
    reg.loadOverrides(saved);
    CVar<int> persisted("test.Persisted", 1, "p", CVarFlags::Persist);
    EXPECT_EQ(persisted.get(), 5);
    EXPECT_EQ(persisted.lastSource(), CVarSource::Config);
    EXPECT_TRUE(reg.saveOverrides().contains("test.PersistStr")) << "pending values survive a save";

    const auto path = (std::filesystem::temp_directory_path() / ("ox_cvars_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json")).string();
    persisted.set(9);
    ASSERT_TRUE(reg.saveOverridesToFile(path));
    persisted.set(1);
    ASSERT_TRUE(reg.loadOverridesFromFile(path));
    EXPECT_EQ(persisted.get(), 9);
    std::filesystem::remove(path);
    CVar<std::string> str("test.PersistStr", "a", "s", CVarFlags::Persist);
    EXPECT_EQ(str.get(), "b");
}

TEST_F(CVars, ScalabilityGroups) {
    namespace sc = ox::scalability;
    CVar<int> shadowRes("test.Shadows.Resolution", 2048, "res", Scalability::Shadows, {512, 1024, 2048, 4096});
    CVar<float> shadowDist("test.Shadows.Distance", 100.0f, "dist", Scalability::Shadows, {50.f, 100.f, 200.f, 400.f});
    CVar<bool> rt("test.RT.Enabled", false, "rt", Scalability::RayTracing, {false, false, true, true});

    sc::setOverall(QualityLevel::Low);
    EXPECT_EQ(shadowRes.get(), 512);
    EXPECT_EQ(shadowDist.get(), 50.f);
    EXPECT_FALSE(rt.get());
    EXPECT_EQ(sc::currentLevel(Scalability::Shadows), QualityLevel::Low);
    EXPECT_EQ(sc::overallLevel(), QualityLevel::Low);

    sc::setGroup(Scalability::Shadows, QualityLevel::Ultra);
    EXPECT_EQ(shadowRes.get(), 4096);
    EXPECT_EQ(sc::currentLevel(Scalability::Shadows), QualityLevel::Ultra);
    EXPECT_EQ(sc::overallLevel(), QualityLevel::Custom);

    // A manual override makes the group Custom; re-applying the level restores it.
    shadowRes.setFromString("1024", CVarSource::Console);
    EXPECT_EQ(sc::currentLevel(Scalability::Shadows), QualityLevel::Custom);
    sc::setGroup(Scalability::Shadows, QualityLevel::Ultra);
    EXPECT_EQ(shadowRes.get(), 4096);

    // The sg.* cvar is the persisted source of truth and applies on change from the console.
    ASSERT_TRUE(CVarRegistry::instance().execute("sg.Shadows Medium"));
    EXPECT_EQ(shadowRes.get(), 1024);
    EXPECT_EQ(sc::currentLevel(Scalability::Shadows), QualityLevel::Medium);

    // Cvars registered later start at the chosen level.
    CVar<int> late("test.Shadows.Cascades", 4, "cascades", Scalability::Shadows, {1, 2, 3, 4});
    EXPECT_EQ(late.get(), 2);

    // Presets round trip, including custom overrides.
    shadowDist.set(123.f, CVarSource::Console);
    const auto preset = sc::savePreset();
    EXPECT_EQ(preset["groups"]["Shadows"], "Medium");
    EXPECT_EQ(preset["overrides"]["test.Shadows.Distance"], 123.0);
    sc::setOverall(QualityLevel::High);
    EXPECT_EQ(shadowRes.get(), 2048);
    ASSERT_TRUE(sc::loadPreset(preset));
    EXPECT_EQ(shadowRes.get(), 1024);
    EXPECT_EQ(shadowDist.get(), 123.f);
    EXPECT_EQ(sc::currentLevel(Scalability::Shadows), QualityLevel::Custom);

    EXPECT_EQ(sc::groupName(Scalability::GlobalIllumination), "GlobalIllumination");
    EXPECT_EQ(sc::groupFromName("Foliage"), Scalability::Foliage);
    EXPECT_EQ(sc::levelFromName("Ultra"), QualityLevel::Ultra);
    sc::setOverall(QualityLevel::High);
}

TEST_F(CVars, DuplicateNameDifferingOnlyInCaseIsRejected) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    CVar<int> a("test.CaseDup", 1, "first");
    EXPECT_DEATH({ CVar<int> b("TEST.casedup", 2, "second"); }, "registered twice");
    EXPECT_DEATH({ ConsoleCommand c("Test.CaseDup", "clash", [](auto) { return std::string{}; }); }, "clashes with cvar");
}

TEST_F(CVars, ChangeListenerSeesEveryChangePath) {
    CVar<int> c("test.Listened", 1, "listened");
    std::vector<std::pair<std::string, CVarSource>> seen;
    auto& reg = CVarRegistry::instance();
    const usize id = reg.addChangeListener([&](ICVar& cv, CVarSource src) {
        if (cv.name() == "test.Listened") seen.emplace_back(cv.toString(), src);
    });
    c.set(2);                                     // code
    ASSERT_TRUE(reg.set("test.listened", "3"));   // registry set (console source by default)
    ASSERT_TRUE(reg.execute("test.Listened 4"));  // console line
    ASSERT_TRUE(reg.execute("test.Listened 4"));  // unchanged: no notification
    reg.loadOverrides({{"test.Listened", 5}});    // config
    reg.removeChangeListener(id);
    c.set(6);
    ASSERT_EQ(seen.size(), 4u);
    EXPECT_EQ(seen[0], (std::pair<std::string, CVarSource>{"2", CVarSource::Code}));
    EXPECT_EQ(seen[1], (std::pair<std::string, CVarSource>{"3", CVarSource::Console}));
    EXPECT_EQ(seen[2], (std::pair<std::string, CVarSource>{"4", CVarSource::Console}));
    EXPECT_EQ(seen[3], (std::pair<std::string, CVarSource>{"5", CVarSource::Config}));
}
