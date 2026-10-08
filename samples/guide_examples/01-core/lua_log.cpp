// Глава 01: логирование из Lua (docs/guide/01-core.md).
#include <oxwald/core/log.hpp>
#include <oxwald/script/script_vm.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

TEST(GuideCoreLua, LogFromScript) {
    std::vector<std::string> lines;
    const int sink = ox::log::addSink([&](const ox::log::Record& r) {
        if (r.category == "script") lines.emplace_back(r.message);
    });

    ox::script::ScriptVM vm;
    sol::environment env = vm.createEnvironment();
    auto result = vm.runFile(std::string(OX_GUIDE_DIR) + "/log_example.lua", &env);
    ox::log::removeSink(sink);

    ASSERT_TRUE(result) << result.error;
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_NE(lines[0].find("player spawned at 1 2 3"), std::string::npos);
    EXPECT_NE(lines[0].find("log_example.lua:"), std::string::npos); // префикс файл:строка
    EXPECT_NE(lines[1].find("hp low: 15"), std::string::npos);
    EXPECT_NE(lines[2].find("print goes to the log too"), std::string::npos);
}
