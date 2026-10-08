#include <oxwald/core/log.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

TEST(Log, SinkReceivesFormattedMessage) {
    std::vector<std::string> got;
    const int id = ox::log::addSink([&](const ox::log::Record& r) { got.emplace_back(r.message); });
    OX_LOG_INFO("test", "value={} name={}", 42, "ox");
    ox::log::removeSink(id);
    OX_LOG_INFO("test", "not captured");
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0], "value=42 name=ox");
}

TEST(Log, MinLevelFilters) {
    int count = 0;
    const int id = ox::log::addSink([&](const ox::log::Record&) { ++count; });
    ox::log::setMinLevel(ox::log::Level::Warn);
    OX_LOG_INFO("test", "filtered");
    OX_LOG_WARN("test", "passes");
    ox::log::setMinLevel(ox::log::Level::Info);
    ox::log::removeSink(id);
    EXPECT_EQ(count, 1);
}
