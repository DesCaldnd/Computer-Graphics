// Глава 01: логирование, OX_ASSERT, Result/Status (docs/guide/01-core.md).
#include <oxwald/core/assert.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/core/result.hpp>

#include <gtest/gtest.h>

#include <charconv>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Ошибка, которую вызывающий может обработать, — Result<T>, а не исключение.
ox::Result<int> parseHealth(std::string_view text) {
    int value = 0;
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        return ox::makeError("'{}' is not a number", text);
    }
    if (value < 0) {
        return ox::makeError("health must be >= 0, got {}", value);
    }
    return value;
}

// Status = Result<void>: «получилось / вот ошибка».
ox::Status applyDamage(int& health, std::string_view amountText) {
    auto amount = parseHealth(amountText);
    if (!amount) {
        return amount.error(); // пробрасываем ошибку наверх
    }
    health -= *amount;
    return {};
}

} // namespace

TEST(GuideCoreLog, SinkReceivesFormattedRecords) {
    std::vector<std::string> lines;
    const int sink = ox::log::addSink([&](const ox::log::Record& r) {
        lines.push_back(std::format("{}|{}|{}", ox::log::levelName(r.level), r.category, r.message));
    });
    const auto previous = ox::log::minLevel();
    ox::log::setMinLevel(ox::log::Level::Info);

    OX_LOG_DEBUG("game", "не попадёт в лог: уровень ниже Info");
    OX_LOG_INFO("game", "player {} joined, hp={}", "Ivan", 100);
    OX_LOG_WARN("game", "low fps: {:.1f}", 24.5);

    ox::log::removeSink(sink);
    ox::log::setMinLevel(previous);

    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0], "info|game|player Ivan joined, hp=100");
    EXPECT_EQ(lines[1], "warn|game|low fps: 24.5");
}

TEST(GuideCoreResult, ValuesAndErrors) {
    auto ok = parseHealth("75");
    ASSERT_TRUE(ok);
    EXPECT_EQ(*ok, 75);

    auto bad = parseHealth("lots");
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().message, "'lots' is not a number");
    EXPECT_EQ(parseHealth("-5").valueOr(0), 0);

    int health = 100;
    EXPECT_TRUE(applyDamage(health, "30"));
    EXPECT_EQ(health, 70);
    ox::Status st = applyDamage(health, "x");
    ASSERT_FALSE(st);
    EXPECT_EQ(health, 70);
}

TEST(GuideCoreAssert, AssertAbortsInEveryBuild) {
    const int players = 2;
    OX_ASSERT(players > 0, "need at least one player, got {}", players); // условие верно — ничего не происходит
    EXPECT_DEATH(OX_ASSERT(players > 5, "need 6 players, got {}", players), "need 6 players, got 2");
}
