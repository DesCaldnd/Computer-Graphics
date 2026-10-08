// Глава 04: объявление CVar'ов, флаги, колбэки, консольные команды, сохранение (docs/guide/04-cvars-quality.md).
#include <oxwald/core/cvar.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace {

// CVar — глобальный (static) объект в .cpp, который точно попадёт в программу (рядом с кодом, который его читает).
ox::CVar<bool> cvGodMode("g.GodMode", false, "Player takes no damage", ox::CVarFlags::Cheat);
ox::CVar<float> cvMouseSensitivity("g.MouseSensitivity", 1.0f, "Mouse sensitivity", 0.1f, 10.0f,
                                   ox::CVarFlags::Persist); // диапазон [0.1, 10], значение зажимается
ox::CVar<int> cvHudMode("g.HudMode", 0, "HUD layout", ox::CVarEnum{"Full", "Minimal", "Off"},
                        ox::CVarFlags::Persist); // int как перечисление: принимает имена
ox::CVar<std::string> cvStartLevel("g.StartLevel", "project://levels/intro.oxscene", "Level to start in");
ox::CVar<int> cvAiThreads("g.AI.Threads", 2, "AI worker threads", ox::CVarFlags::RequiresRestart);
ox::CVar<int> cvBuildNumber("g.BuildNumber", 1234, "Build number", ox::CVarFlags::ReadOnly);

int g_gold = 0;
// Консольная команда: аргументы без имени команды, возвращаемая строка печатается в консоли.
ox::ConsoleCommand cmdGiveGold("g.GiveGold", "Give gold: g.GiveGold <amount>",
                               [](std::span<const std::string> args) -> std::string {
                                   if (args.empty()) return "usage: g.GiveGold <amount>";
                                   g_gold += std::stoi(args[0]);
                                   return "gold = " + std::to_string(g_gold);
                               });

struct GuideCVars : ::testing::Test {
    void SetUp() override { ox::CVarRegistry::instance().setCheatsEnabled(false); }
};

} // namespace

TEST_F(GuideCVars, ReadSetAndReact) {
    // Чтение — дешёвое (атомик), из любого потока.
    float sensitivity = cvMouseSensitivity; // неявное преобразование == get()
    EXPECT_FLOAT_EQ(sensitivity, 1.0f);

    float applied = 0.0f;
    const auto id = cvMouseSensitivity.onChanged([&](const float& now, const float& /*before*/) { applied = now; });
    cvMouseSensitivity.set(2.5f);
    EXPECT_FLOAT_EQ(applied, 2.5f);
    cvMouseSensitivity.set(50.0f); // вне диапазона -> 10
    EXPECT_FLOAT_EQ(cvMouseSensitivity.get(), 10.0f);
    cvMouseSensitivity.removeCallback(id);
    cvMouseSensitivity.reset();

    EXPECT_EQ(cvHudMode.toString(), "Full");
    EXPECT_TRUE(cvHudMode.setFromString("minimal", ox::CVarSource::Console)); // регистр не важен
    EXPECT_EQ(cvHudMode.get(), 1);
    cvHudMode.reset();
}

TEST_F(GuideCVars, ConsoleLinesAndPermissions) {
    auto& reg = ox::CVarRegistry::instance();

    // Строка консоли: "имя" — показать, "имя значение" — установить, иначе — команда.
    ASSERT_TRUE(reg.execute("g.StartLevel \"project://levels/boss arena.oxscene\""));
    EXPECT_EQ(cvStartLevel.get(), "project://levels/boss arena.oxscene");
    auto shown = reg.execute("g.StartLevel");
    ASSERT_TRUE(shown);
    EXPECT_NE(shown->find("boss arena"), std::string::npos);

    auto out = reg.execute("g.GiveGold 150");
    ASSERT_TRUE(out);
    EXPECT_EQ(*out, "gold = 150");

    // Cheat: из консоли — только когда читы включены.
    EXPECT_FALSE(reg.execute("g.GodMode 1"));
    reg.setCheatsEnabled(true);
    EXPECT_TRUE(reg.execute("g.GodMode 1"));
    EXPECT_TRUE(cvGodMode.get());
    cvGodMode.reset();

    // ReadOnly: менять может только код.
    EXPECT_FALSE(reg.execute("g.BuildNumber 1"));
    EXPECT_EQ(cvBuildNumber.get(), 1234);

    // RequiresRestart: значение сохраняется, но действует после перезапуска.
    reg.clearRestartRequired();
    ASSERT_TRUE(reg.execute("g.AI.Threads 4"));
    EXPECT_TRUE(reg.restartRequired());
    reg.clearRestartRequired();
    cvAiThreads.reset();

    // Автодополнение и поиск.
    EXPECT_EQ(reg.complete("g.Mouse"), std::vector<std::string>{"g.MouseSensitivity"});
    EXPECT_EQ(reg.findAs<float>("g.MouseSensitivity"), &cvMouseSensitivity);
    ASSERT_TRUE(reg.execute("g.MouseSensitivity 2"));
    EXPECT_EQ(cvMouseSensitivity.lastSource(), ox::CVarSource::Console); // кто менял последним
    cvMouseSensitivity.reset();
}

TEST_F(GuideCVars, PersistOverrides) {
    auto& reg = ox::CVarRegistry::instance();
    cvMouseSensitivity.set(3.0f);
    cvHudMode.set(2);

    // В файл попадают только Persist-переменные, отличные от значения по умолчанию.
    const nlohmann::json saved = reg.saveOverrides();
    EXPECT_EQ(saved["g.MouseSensitivity"], 3.0f);
    EXPECT_EQ(saved["g.HudMode"], 2);
    EXPECT_FALSE(saved.contains("g.StartLevel"));

    const auto path = std::filesystem::temp_directory_path() / "oxwald_guide_cvars.json";
    ASSERT_TRUE(reg.saveOverridesToFile(path.string()));
    cvMouseSensitivity.reset();
    cvHudMode.reset();
    ASSERT_TRUE(reg.loadOverridesFromFile(path.string()));
    EXPECT_FLOAT_EQ(cvMouseSensitivity.get(), 3.0f);
    EXPECT_EQ(cvMouseSensitivity.lastSource(), ox::CVarSource::Config);
    std::filesystem::remove(path);
    cvMouseSensitivity.reset();
    cvHudMode.reset();
}
