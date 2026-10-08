// Глава 04: внутриигровая консоль (runtime Console) (docs/guide/04-cvars-quality.md).
#include <oxwald/runtime/console.hpp>
#include <oxwald/runtime/engine.hpp>
#include <oxwald/runtime/settings.hpp>

#include <gtest/gtest.h>

#include <filesystem>

TEST(GuideConsole, CommandsCompletionHistory) {
    ox::Console console;
    int spawned = 0;
    // Команда, живущая столько же, сколько эта консоль (в отличие от глобальной ox::ConsoleCommand).
    console.addCommand("spawn", "spawn <prefab> [count]", [&](std::span<const std::string> args) -> std::string {
        if (args.empty()) return "usage: spawn <prefab> [count]";
        const int count = args.size() > 1 ? std::stoi(args[1]) : 1;
        spawned += count;
        return "spawned " + std::to_string(count) + " x " + args[0];
    });

    auto r = console.execute("spawn \"orc warrior\" 3");
    ASSERT_TRUE(r);
    EXPECT_EQ(*r, "spawned 3 x orc warrior");

    // Несколько команд через `;`, присваивание cvar'ов (здесь — cvar'ы рантайма).
    ASSERT_TRUE(console.execute("r.VSync false; t.MaxFPS 120"));
    EXPECT_FALSE(ox::cvars::vsync().get());
    EXPECT_EQ(ox::cvars::maxFps().get(), 120);

    // Ошибки — как Result-ошибки, консоль печатает их красным.
    EXPECT_FALSE(console.execute("r.NoSuchVariable 1"));

    // Автодополнение имён и значений (enum/bool) — это то, что делает Tab.
    EXPECT_EQ(console.complete("r.WindowMode F"), std::vector<std::string>{"r.WindowMode Fullscreen"});
    EXPECT_EQ(console.completeCommonPrefix("spa"), "spawn");

    // История (стрелки вверх/вниз).
    EXPECT_EQ(console.historyPrev(), "r.NoSuchVariable 1");
    EXPECT_EQ(console.historyPrev(), "r.VSync false; t.MaxFPS 120");

    // Встроенные команды: help [prefix], find <text>, history, clear.
    auto help = console.execute("help t.Max");
    ASSERT_TRUE(help);
    EXPECT_NE(help->find("t.MaxFPS"), std::string::npos);

    ox::cvars::vsync().reset();
    ox::cvars::maxFps().reset();
}

TEST(GuideConsole, EngineConsoleControlsTheGame) {
    const auto userDir = std::filesystem::temp_directory_path() / "oxwald_guide_console";
    ox::Engine engine;
    ox::EngineConfig config;
    config.headless = true;
    config.userDir = userDir;
    config.saveUserSettingsOnShutdown = false;
    ASSERT_TRUE(engine.init(config));

    // У движка своя консоль с командами quit pause step timescale level save load stats ...
    ASSERT_TRUE(engine.console().execute("timescale 0.5"));
    EXPECT_DOUBLE_EQ(engine.timeScale(), 0.5);
    ASSERT_TRUE(engine.console().execute("pause"));
    EXPECT_TRUE(engine.paused());
    ASSERT_TRUE(engine.console().execute("quit"));
    EXPECT_FALSE(engine.tick(1.0 / 60.0)); // кадр после quit сообщает, что пора выходить

    engine.shutdown();
    std::filesystem::remove_all(userDir);
}
