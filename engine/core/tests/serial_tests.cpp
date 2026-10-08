#include <oxwald/core/serial/archive.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>

using namespace ox;

namespace {

enum class Mode : i32 { Off = 0, Low = 1, High = 2 };
enum class Flags : u32 { None = 0, A = 1, B = 2 };

struct Leaf {
    f32 a = 0.0f;
    std::string s;
    friend bool operator==(const Leaf&, const Leaf&) = default;
};

struct Everything {
    bool b = false;
    i8 i8v = 0;
    i16 i16v = 0;
    i32 i32v = 0;
    i64 i64v = 0;
    u8 u8v = 0;
    u16 u16v = 0;
    u32 u32v = 0;
    u64 u64v = 0;
    f32 f = 0.0f;
    f64 d = 0.0;
    std::string str;
    glm::vec2 v2{0};
    glm::vec3 v3{0};
    glm::vec4 v4{0};
    glm::ivec2 iv2{0};
    glm::ivec3 iv3{0};
    glm::ivec4 iv4{0};
    glm::quat q{1, 0, 0, 0};
    glm::mat4 m{1.0f};
    Uuid id;
    Mode mode = Mode::Off;
    Flags flags = Flags::None; // enum without registered names
    Leaf leaf;
    std::vector<f32> floats;
    std::vector<glm::vec3> points;
    std::vector<std::string> names;
    std::vector<Leaf> leaves;
    std::vector<std::vector<i32>> nested;
    std::vector<u8> bytes;
    std::optional<i32> optSet;
    std::optional<i32> optEmpty;
    std::optional<Leaf> optLeaf;
    std::map<std::string, f32> map;
    std::unordered_map<std::string, Leaf> umap;
    std::vector<Mode> modes;

    friend bool operator==(const Everything&, const Everything&) = default;
};

Everything makeEverything() {
    Everything e;
    e.b = true;
    e.i8v = -7;
    e.i16v = -30000;
    e.i32v = -2000000000;
    e.i64v = std::numeric_limits<i64>::min() + 5;
    e.u8v = 250;
    e.u16v = 65000;
    e.u32v = 4000000000u;
    e.u64v = std::numeric_limits<u64>::max() - 3;
    e.f = 0.1f;
    e.d = 1.0 / 3.0;
    e.str = "héllo \"world\"\n";
    e.v2 = {1.5f, -2.5f};
    e.v3 = {0.1f, 0.2f, 0.3f};
    e.v4 = {1, 2, 3, 4};
    e.iv2 = {-1, 2};
    e.iv3 = {3, -4, 5};
    e.iv4 = {6, 7, -8, 9};
    e.q = glm::normalize(glm::quat(0.9f, 0.1f, 0.2f, 0.3f));
    e.m = glm::mat4(1.0f);
    e.m[3] = glm::vec4(10, 20, 30, 1);
    e.m[0][1] = 0.25f;
    e.id = Uuid::generate();
    e.mode = Mode::High;
    e.flags = static_cast<Flags>(3);
    e.leaf = {3.25f, "leaf"};
    e.floats = {1.0f, 2.0f, 1e-30f, -0.0f};
    e.points = {{1, 2, 3}, {4, 5, 6}};
    e.names = {"a", "", "c"};
    e.leaves = {{1, "x"}, {2, "y"}};
    e.nested = {{1, 2}, {}, {3}};
    e.bytes = {0, 1, 2, 255, 128};
    e.optSet = 17;
    e.optLeaf = Leaf{9, "opt"};
    e.map = {{"one", 1.0f}, {"two", 2.0f}};
    e.umap = {{"k2", {2, "b"}}, {"k1", {1, "a"}}};
    e.modes = {Mode::Low, Mode::Off, Mode::High};
    return e;
}

void registerSerialTestTypes() {
    OX_REFLECT_ENUM(Mode, "Test.Mode").value("Off", Mode::Off).value("Low", Mode::Low).value("High", Mode::High);
    OX_REFLECT_TYPE(Leaf, "Test.Leaf").field("a", &Leaf::a).field("s", &Leaf::s);
    OX_REFLECT_TYPE(Everything, "Test.Everything")
        .field("b", &Everything::b)
        .field("i8", &Everything::i8v)
        .field("i16", &Everything::i16v)
        .field("i32", &Everything::i32v)
        .field("i64", &Everything::i64v)
        .field("u8", &Everything::u8v)
        .field("u16", &Everything::u16v)
        .field("u32", &Everything::u32v)
        .field("u64", &Everything::u64v)
        .field("f", &Everything::f)
        .field("d", &Everything::d)
        .field("str", &Everything::str)
        .field("v2", &Everything::v2)
        .field("v3", &Everything::v3)
        .field("v4", &Everything::v4)
        .field("iv2", &Everything::iv2)
        .field("iv3", &Everything::iv3)
        .field("iv4", &Everything::iv4)
        .field("q", &Everything::q)
        .field("m", &Everything::m)
        .field("id", &Everything::id)
        .field("mode", &Everything::mode)
        .field("flags", &Everything::flags)
        .field("leaf", &Everything::leaf)
        .field("floats", &Everything::floats)
        .field("points", &Everything::points)
        .field("names", &Everything::names)
        .field("leaves", &Everything::leaves)
        .field("nested", &Everything::nested)
        .field("bytes", &Everything::bytes)
        .field("optSet", &Everything::optSet)
        .field("optEmpty", &Everything::optEmpty)
        .field("optLeaf", &Everything::optLeaf)
        .field("map", &Everything::map)
        .field("umap", &Everything::umap)
        .field("modes", &Everything::modes);
}

// Two versions of the same logical type for schema evolution tests.
struct SettingsV1 {
    i32 volume = 5;
    f32 brightness = 0.5f;
    std::string playerName = "anon";
    i32 obsolete = 99;
};
struct SettingsV2 {
    f64 volume = 1.0;           // type widened
    f32 brightness = 0.0f;      // unchanged
    std::string displayName;    // renamed from playerName
    bool vsync = true;          // new field keeps default
};

struct Serial : ::testing::Test {
    void SetUp() override {
        registerSerialTestTypes();
        OX_REFLECT_TYPE(SettingsV1, "Settings")
            .field("volume", &SettingsV1::volume)
            .field("brightness", &SettingsV1::brightness)
            .field("playerName", &SettingsV1::playerName)
            .field("obsolete", &SettingsV1::obsolete);
        OX_REFLECT_TYPE(SettingsV2, "SettingsV2")
            .field("volume", &SettingsV2::volume)
            .field("brightness", &SettingsV2::brightness)
            .field("displayName", &SettingsV2::displayName, attr::FormerName{"playerName"})
            .field("vsync", &SettingsV2::vsync);
    }
};

} // namespace

TEST_F(Serial, ValueTreeBasics) {
    auto arr = serial::Value::makeArray();
    arr.push(serial::Value::makeVec3({1, 2, 3}));
    arr.push(serial::Value::makeVec3({4, 5, 6}));
    EXPECT_TRUE(arr.isPacked());
    EXPECT_EQ(arr.size(), 2u);
    EXPECT_EQ(glm::vec3(arr.at(1).getVec()), glm::vec3(4, 5, 6));
    EXPECT_EQ(arr.desc()->str(), "array<vec3>");
    EXPECT_EQ(serial::TypeDesc::parse("map<optional<array<quat>>>")->str(), "map<optional<array<quat>>>");
    EXPECT_EQ(serial::TypeDesc::parse("array<"), nullptr);
    EXPECT_EQ(serial::TypeDesc::parse("banana"), nullptr);
    EXPECT_EQ(serial::Value::makeInt(-3, serial::Tag::I8).getInt(), -3);
    EXPECT_EQ(serial::Value::makeUInt(200, serial::Tag::U8).getInt(), 200);
    const std::vector<std::byte> blob = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    EXPECT_EQ(*serial::base64Decode(serial::base64Encode(blob)), blob);
    EXPECT_EQ(serial::base64Encode(std::span(blob).first(1)), "AQ==");
}

TEST_F(Serial, ReflectedRoundTripBinaryAndJson) {
    const Everything src = makeEverything();
    serial::Writer w("test", 7);
    w.value("everything", src);
    const auto doc = std::move(w).finish();

    // binary
    auto bin = serial::encodeBinary(doc);
    auto decoded = serial::decodeBinary(bin);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded->version, 7u);
    EXPECT_EQ(decoded->kind, "test");
    EXPECT_EQ(*decoded, doc);
    serial::Reader r(*decoded);
    Everything back;
    ASSERT_TRUE(r.value("everything", back));
    EXPECT_EQ(back, src);

    // json
    const std::string json = serial::toJsonString(doc);
    auto fromJson = serial::parseJsonString(json);
    ASSERT_TRUE(fromJson) << fromJson.error().message;
    EXPECT_EQ(*fromJson, doc);
    Everything back2;
    ASSERT_TRUE(serial::Reader(*fromJson).value("everything", back2));
    EXPECT_EQ(back2, src);
    EXPECT_NE(json.find("\"mode\": \"High\""), std::string::npos) << "enums are written by name in JSON";
}

TEST_F(Serial, BinaryJsonBinaryIsByteIdentical) {
    serial::Writer w("scene", 3);
    w.value("a", makeEverything());
    w.beginArray("objects");
    for (int i = 0; i < 3; ++i) {
        w.beginObject({}, i == 1 ? "Other" : "");
        w.value("index", i);
        if (i == 2) w.value("extra", std::string("only here"));
        w.endObject();
    }
    w.endArray();
    // Same type name with an incompatible shape forces a second schema entry ("Test.Leaf#2").
    w.beginObject("weird", "Test.Leaf");
    w.value("a", std::string("not a float"));
    w.endObject();
    w.blob("blob", std::as_bytes(std::span("raw\0data", 8)));
    const auto bin = w.toBinary();

    auto json = serial::binaryToJson(bin);
    ASSERT_TRUE(json) << json.error().message;
    auto bin2 = serial::jsonToBinary(*json);
    ASSERT_TRUE(bin2) << bin2.error().message;
    EXPECT_EQ(bin, *bin2);
    EXPECT_NE(json->find("Test.Leaf#2"), std::string::npos);
}

TEST_F(Serial, NonFiniteFloatsSurviveJson) {
    serial::Writer w;
    w.value("nan", std::numeric_limits<f32>::quiet_NaN());
    w.value("inf", std::numeric_limits<f64>::infinity());
    w.value("ninf", -std::numeric_limits<f32>::infinity());
    auto doc = serial::parseJsonString(w.toJson());
    ASSERT_TRUE(doc);
    serial::Reader r(*doc);
    f32 nan = 0;
    f64 inf = 0;
    f32 ninf = 0;
    r.value("nan", nan);
    r.value("inf", inf);
    r.value("ninf", ninf);
    EXPECT_TRUE(std::isnan(nan));
    EXPECT_TRUE(std::isinf(inf) && inf > 0);
    EXPECT_TRUE(std::isinf(ninf) && ninf < 0);
}

TEST_F(Serial, SchemaEvolutionAddRemoveRenameRetype) {
    SettingsV1 v1;
    v1.volume = 8;
    v1.brightness = 0.75f;
    v1.playerName = "Oxwald";
    serial::Writer w("settings", 1);
    w.value("settings", v1);
    const auto bin = w.toBinary();

    auto r = serial::Reader::fromBytes(bin);
    ASSERT_TRUE(r);
    SettingsV2 v2;
    ASSERT_TRUE(r->value("settings", v2));
    EXPECT_DOUBLE_EQ(v2.volume, 8.0);        // i32 -> f64
    EXPECT_FLOAT_EQ(v2.brightness, 0.75f);   // same
    EXPECT_EQ(v2.displayName, "Oxwald");     // renamed via FormerName
    EXPECT_TRUE(v2.vsync);                   // missing -> default; "obsolete" ignored

    // And backwards: a newer file read by the old type.
    serial::Writer w2;
    SettingsV2 newer;
    newer.volume = 3.9;
    newer.displayName = "x";
    w2.value("settings", newer);
    SettingsV1 old;
    ASSERT_TRUE(serial::Reader::fromBytes(w2.toBinary())->value("settings", old));
    EXPECT_EQ(old.volume, 3);              // f64 -> i32 truncation
    EXPECT_EQ(old.playerName, "anon");     // new name unknown to old type: default kept
    EXPECT_EQ(old.obsolete, 99);
}

TEST_F(Serial, CorruptionIsDetected) {
    serial::Writer w;
    w.value("x", makeEverything());
    auto bin = w.toBinary();
    auto info = serial::inspectBinary(bin);
    ASSERT_TRUE(info);
    const auto& data = info->chunks.back();
    ASSERT_EQ(data.id, "DATA");
    bin[data.offset + data.size / 2] ^= std::byte{0x5A};
    auto bad = serial::decodeBinary(bin);
    ASSERT_FALSE(bad);
    EXPECT_NE(bad.error().message.find("CRC"), std::string::npos);
    auto info2 = serial::inspectBinary(bin);
    ASSERT_TRUE(info2);
    EXPECT_FALSE(info2->chunks.back().crcValid);

    auto truncated = std::vector<std::byte>(bin.begin(), bin.begin() + 30);
    EXPECT_FALSE(serial::decodeBinary(truncated));
    EXPECT_FALSE(serial::decodeBinary(std::vector<std::byte>(3, std::byte{0})));
}

TEST_F(Serial, UnknownFieldDescriptorIsSkippedBySize) {
    serial::Writer w;
    w.beginObject("o", "T");
    w.value("keep", 5);
    w.value("future", 6);
    w.endObject();
    auto bin = w.toBinary();
    // Simulate a newer writer: rename the descriptor text "i32" of the string table to an unknown tag of the same
    // length ("x32"), then fix the STRS CRC. Both fields become unknown, so use a distinct type for "keep".
    auto info = serial::inspectBinary(bin);
    ASSERT_TRUE(info);
    const auto& strs = info->chunks[1];
    ASSERT_EQ(strs.id, "STRS");
    const std::string needle = "i32";
    auto* begin = reinterpret_cast<char*>(bin.data()) + strs.offset;
    std::string_view region(begin, strs.size);
    const auto pos = region.find(needle);
    ASSERT_NE(pos, std::string_view::npos);
    begin[pos] = 'x';
    const u32 crc = crc32(std::span(bin).subspan(strs.offset, strs.size));
    std::memcpy(bin.data() + strs.offset - 4, &crc, 4);
    auto doc = serial::decodeBinary(bin);
    ASSERT_TRUE(doc) << doc.error().message;
    const auto* o = doc->root.find("o");
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->size(), 0u); // both i32 fields skipped, object still decodes
}

TEST_F(Serial, ManualApiAndReader) {
    serial::Writer w("save", 2);
    w.value("level", std::string("forest"));
    w.beginObject("quests");
    w.value("active", std::vector<i32>{3, 4});
    w.beginArray("log");
    w.element(std::string("started"));
    w.element(std::string("met npc"));
    w.endArray();
    w.endObject();
    w.beginMap("inventory");
    w.value("sword", 1);
    w.value("potion", 5);
    w.endMap();
    w.beginArray("enemies");
    w.beginObject({}, "Enemy");
    w.value("hp", 10.0f);
    w.endObject();
    w.endArray();
    std::vector<std::byte> thumb(1000);
    for (usize i = 0; i < thumb.size(); ++i) thumb[i] = std::byte(i * 7);
    w.blob("thumb", thumb);

    for (const auto& bytes : {w.toBinary(), [&] {
             auto s = w.toJson();
             return std::vector<std::byte>(reinterpret_cast<const std::byte*>(s.data()),
                                           reinterpret_cast<const std::byte*>(s.data()) + s.size());
         }()}) {
        auto r = serial::Reader::fromBytes(bytes);
        ASSERT_TRUE(r) << r.error().message;
        EXPECT_EQ(r->version(), 2u);
        EXPECT_EQ(r->kind(), "save");
        std::string level;
        EXPECT_TRUE(r->value("level", level));
        EXPECT_EQ(level, "forest");
        ASSERT_TRUE(r->beginObject("quests"));
        std::vector<i32> active;
        EXPECT_TRUE(r->value("active", active));
        EXPECT_EQ(active, (std::vector<i32>{3, 4}));
        auto n = r->beginArray("log");
        ASSERT_TRUE(n);
        EXPECT_EQ(*n, 2u);
        std::string entry;
        EXPECT_TRUE(r->value(1, entry));
        EXPECT_EQ(entry, "met npc");
        r->endArray();
        r->endObject();
        std::map<std::string, i32> inv;
        EXPECT_TRUE(r->value("inventory", inv));
        EXPECT_EQ(inv["potion"], 5);
        ASSERT_EQ(r->beginArray("enemies").value_or(0), 1u);
        ASSERT_TRUE(r->beginObjectAt(0));
        f32 hp = 0;
        EXPECT_TRUE(r->value("hp", hp));
        EXPECT_EQ(hp, 10.0f);
        r->endObject();
        r->endArray();
        EXPECT_EQ(*r->blob("thumb"), thumb);
        EXPECT_FALSE(r->has("missing"));
        i32 untouched = 42;
        EXPECT_FALSE(r->value("missing", untouched));
        EXPECT_EQ(untouched, 42);
    }
}

TEST_F(Serial, HandWrittenJsonWithoutSchema) {
    const char* text = R"({
        "volume": 7,
        "brightness": 0.25,
        "playerName": "hand",
        "unknown": [1, 2, 3]
    })";
    auto doc = serial::parseJsonString(text);
    ASSERT_TRUE(doc) << doc.error().message;
    SettingsV2 s;
    ASSERT_TRUE(serial::fromValue(doc->root, s));
    EXPECT_DOUBLE_EQ(s.volume, 7.0);
    EXPECT_FLOAT_EQ(s.brightness, 0.25f);
    EXPECT_EQ(s.displayName, "hand");

    const char* vecs = R"({"v3": [1, 2, 3.5], "q": [0, 0, 0, 1], "mode": "Low", "id": "00000000-0000-0000-0000-00000000002a",
                         "optSet": null, "points": [[1,2,3],[4,5,6]], "leaf": {"a": 2, "s": "x"}})";
    auto doc2 = serial::parseJsonString(vecs);
    ASSERT_TRUE(doc2) << doc2.error().message;
    Everything e;
    e.optSet = 3;
    ASSERT_TRUE(serial::fromValue(doc2->root, e));
    EXPECT_EQ(e.v3, glm::vec3(1, 2, 3.5f));
    EXPECT_EQ(e.q, glm::quat(1, 0, 0, 0));
    EXPECT_EQ(e.mode, Mode::Low);
    EXPECT_EQ(e.id.lo, 42u);
    EXPECT_FALSE(e.optSet.has_value());
    ASSERT_EQ(e.points.size(), 2u);
    EXPECT_EQ(e.points[1], glm::vec3(4, 5, 6));
    EXPECT_EQ(e.leaf.s, "x");

    EXPECT_FALSE(serial::parseJsonString("{ not json"));
    EXPECT_FALSE(serial::parseJsonString(R"({"mixed": [1, "a"]})"));
}

TEST_F(Serial, FieldFilterAndNoSerialize) {
    struct Filtered {
        i32 saved = 1;
        i32 transient = 2;
        i32 never = 3;
    };
    OX_REFLECT_TYPE(Filtered, "Test.Filtered")
        .field("saved", &Filtered::saved, attr::SaveGame{})
        .field("transient", &Filtered::transient)
        .field("never", &Filtered::never, attr::NoSerialize{});
    Filtered f;
    const auto all = serial::toValue(f);
    EXPECT_EQ(all.size(), 2u);
    serial::ConvertOptions saveOnly;
    saveOnly.fieldFilter = [](const reflect::FieldInfo& fi) { return fi.attributes.saveGame; };
    const auto saved = serial::toValue(f, saveOnly);
    ASSERT_EQ(saved.size(), 1u);
    EXPECT_EQ(saved.fields()[0].first, "saved");
}

TEST_F(Serial, SaveAndLoadFilesAtomically) {
    const auto dir = std::filesystem::temp_directory_path() / ("ox_serial_" + Uuid::generate().toString());
    serial::Writer w("test", 1);
    w.value("e", makeEverything());
    ASSERT_TRUE(w.save(dir / "a.oxb"));
    ASSERT_TRUE(w.save(dir / "a.oxb.json"));
    EXPECT_FALSE(std::filesystem::exists(dir / "a.oxb.tmp"));
    auto a = serial::loadDocument(dir / "a.oxb");
    auto b = serial::loadDocument(dir / "a.oxb.json");
    ASSERT_TRUE(a && b);
    EXPECT_EQ(*a, *b);
    EXPECT_FALSE(serial::loadDocument(dir / "missing.oxb"));
    std::filesystem::remove_all(dir);
}

TEST_F(Serial, PartialBinaryDecodeSkipsUnrequestedRootFields) {
    serial::Writer w("save", 1);
    w.beginObject("header");
    w.value("slot", std::string("a"));
    w.endObject();
    w.value("everything", makeEverything());
    w.beginArray("big");
    for (i32 i = 0; i < 1000; ++i) w.element(std::string("entity ") + std::to_string(i));
    w.endArray();
    const auto bin = w.toBinary();

    auto partial = serial::decodeBinary(bin, serial::BinaryDecodeOptions{{"header"}});
    ASSERT_TRUE(partial) << partial.error().message;
    ASSERT_NE(partial->root.find("header"), nullptr);
    EXPECT_EQ(partial->root.find("everything"), nullptr);
    EXPECT_EQ(partial->root.find("big"), nullptr);
    EXPECT_EQ(partial->root.find("header")->find("slot")->getString(), "a");

    auto full = serial::decodeBinary(bin, serial::BinaryDecodeOptions{});
    ASSERT_TRUE(full);
    EXPECT_NE(full->root.find("big"), nullptr) << "empty field list = full decode";
}
