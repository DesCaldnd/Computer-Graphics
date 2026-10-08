#include <oxwald/core/hash.hpp>
#include <oxwald/core/math.hpp>
#include <oxwald/core/reflect.hpp>
#include <oxwald/core/result.hpp>
#include <oxwald/core/serial/convert.hpp>
#include <oxwald/core/services.hpp>
#include <oxwald/core/uuid.hpp>

#include <gtest/gtest.h>

#include <string>
#include <unordered_set>
#include <vector>

using namespace ox;
using namespace ox::literals;

TEST(Hash, KnownValues) {
    EXPECT_EQ(fnv1a64(""), 0xcbf29ce484222325ull);
    EXPECT_EQ(fnv1a64("a"), 0xaf63dc4c8601ec8cull);
    static_assert("hello"_hash == fnv1a64("hello"));
    const char check[] = "123456789";
    EXPECT_EQ(crc32(check, 9), 0xCBF43926u);
    // Incremental CRC equals one-shot CRC.
    EXPECT_EQ(crc32(check + 4, 5, crc32(check, 4)), 0xCBF43926u);
    EXPECT_NE(hashCombine(1, 2), hashCombine(2, 1));
}

TEST(Uuid, GenerateParseFormat) {
    std::unordered_set<Uuid> seen;
    for (int i = 0; i < 1000; ++i) {
        const Uuid id = Uuid::generate();
        EXPECT_TRUE(id.isValid());
        EXPECT_EQ((id.hi >> 12) & 0xF, 4u) << "version 4";
        EXPECT_EQ(id.lo >> 62, 2u) << "RFC 4122 variant";
        EXPECT_TRUE(seen.insert(id).second);
    }
    const Uuid id = Uuid::generate();
    const std::string s = id.toString();
    EXPECT_EQ(s.size(), 36u);
    EXPECT_EQ(Uuid::parse(s), id);
    std::string compact = s;
    std::erase(compact, '-');
    EXPECT_EQ(Uuid::parse(compact), id);
    EXPECT_FALSE(Uuid::parse("not-a-uuid"));
    EXPECT_FALSE(Uuid::parse("0123456789abcdef0123456789abcdeg"));
    EXPECT_FALSE(Uuid::parse(s.substr(1)));
    EXPECT_EQ(Uuid::fromName("x"), Uuid::fromName("x"));
    EXPECT_NE(Uuid::fromName("x"), Uuid::fromName("y"));
    EXPECT_TRUE(Uuid{}.isNil());
    EXPECT_EQ(Uuid{}.toString(), "00000000-0000-0000-0000-000000000000");
}

namespace {
Result<int> parsePositive(int v) {
    if (v <= 0) return makeError("{} is not positive", v);
    return v;
}
Status doThing(bool ok) {
    if (!ok) return Error{"nope"};
    return {};
}
} // namespace

TEST(Result, ValueAndError) {
    auto ok = parsePositive(3);
    ASSERT_TRUE(ok);
    EXPECT_EQ(*ok, 3);
    auto bad = parsePositive(-1);
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().message, "-1 is not positive");
    EXPECT_EQ(bad.valueOr(7), 7);
    EXPECT_TRUE(doThing(true));
    EXPECT_EQ(doThing(false).error().message, "nope");
    Result<std::vector<int>> moved = std::vector<int>{1, 2};
    auto v = std::move(moved).value();
    EXPECT_EQ(v.size(), 2u);
}

namespace {
struct ILog {
    virtual ~ILog() = default;
    virtual std::string name() const = 0;
};
struct Order {
    std::vector<std::string> destroyed;
};
struct ConsoleLog : ILog {
    explicit ConsoleLog(Order& o) : order(o) {}
    ~ConsoleLog() override { order.destroyed.push_back("log"); }
    std::string name() const override { return "console"; }
    Order& order;
};
struct Physics {
    explicit Physics(Order& o) : order(o) {}
    ~Physics() { order.destroyed.push_back("physics"); }
    Order& order;
};
} // namespace

TEST(Services, AddGetRemoveAndReverseDestruction) {
    Order order;
    {
        Services s;
        s.add<ILog>(std::make_unique<ConsoleLog>(order));
        s.emplace<Physics>(order);
        EXPECT_EQ(s.get<ILog>().name(), "console");
        EXPECT_NE(s.tryGet<Physics>(), nullptr);
        EXPECT_EQ(s.tryGet<int>(), nullptr);
        EXPECT_TRUE(s.has<ILog>());
        EXPECT_EQ(s.size(), 2u);
        int external = 5;
        s.addExternal(external);
        EXPECT_EQ(&s.get<int>(), &external);
        EXPECT_TRUE(s.remove<int>());
        EXPECT_FALSE(s.remove<int>());
    }
    EXPECT_EQ(order.destroyed, (std::vector<std::string>{"physics", "log"}));

    Order o2;
    Services a;
    a.emplace<Physics>(o2);
    Services b = std::move(a);
    EXPECT_TRUE(b.has<Physics>());
    EXPECT_TRUE(b.remove<Physics>());
    EXPECT_EQ(o2.destroyed.size(), 1u);
}

TEST(CoreReflection, TransformIsReflected) {
    reflect::registerCoreReflection();
    Transform t;
    t.position = {1, 2, 3};
    const auto v = serial::toValue(t);
    EXPECT_EQ(v.typeName(), "ox.Transform");
    Transform back;
    ASSERT_TRUE(serial::fromValue(v, back));
    EXPECT_EQ(back, t);
}
