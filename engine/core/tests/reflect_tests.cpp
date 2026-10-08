#include <oxwald/core/reflect.hpp>

#include <gtest/gtest.h>

using namespace ox;

namespace {

enum class Shape : u8 { Box, Sphere, Capsule };

struct Inner {
    f32 weight = 1.0f;
    glm::vec3 offset{0.0f};
};

struct BaseThing {
    std::string label = "base";
};

struct Thing : BaseThing {
    i32 count = 3;
    Shape shape = Shape::Sphere;
    glm::vec3 position{1, 2, 3};
    glm::quat rotation{1, 0, 0, 0};
    Inner inner;
    std::vector<Inner> list;
    std::optional<f32> maybe;
    std::map<std::string, i32> tags;
    f32 hiddenValue = 0.0f;
};

void registerTestTypes() {
    OX_REFLECT_ENUM(Shape, "Test.Shape").value("Box", Shape::Box).value("Sphere", Shape::Sphere).value("Capsule", Shape::Capsule);
    OX_REFLECT_TYPE(Inner, "Test.Inner").field("weight", &Inner::weight, ox::attr::Range{0.0, 10.0}).field("offset", &Inner::offset);
    OX_REFLECT_TYPE(BaseThing, "Test.BaseThing").field("label", &BaseThing::label);
    OX_REFLECT_TYPE(Thing, "Test.Thing")
        .base<BaseThing>()
        .attributes(ox::attr::Category{"Testing"})
        .field("count", &Thing::count, ox::attr::DisplayName{"Count"}, ox::attr::Tooltip{"How many"},
               ox::attr::Step{1.0}, ox::attr::SaveGame{})
        .field("shape", &Thing::shape)
        .field("position", &Thing::position, ox::attr::Replicated{})
        .field("rotation", &Thing::rotation)
        .field("inner", &Thing::inner)
        .field("list", &Thing::list)
        .field("maybe", &Thing::maybe)
        .field("tags", &Thing::tags)
        .field("hiddenValue", &Thing::hiddenValue, ox::attr::Hidden{}, ox::attr::ReadOnly{}, ox::attr::NoSerialize{},
               ox::attr::FormerName{"secret"}, ox::attr::Meta{"icon", "eye"});
}

struct Reflected : ::testing::Test {
    void SetUp() override { registerTestTypes(); }
};

} // namespace

TEST_F(Reflected, BuiltinsHaveStableNames) {
    EXPECT_EQ(reflect::typeOf<f32>().name, "f32");
    EXPECT_EQ(reflect::typeOf<u16>().name, "u16");
    EXPECT_EQ(reflect::typeOf<glm::vec3>().kind, reflect::Kind::Math);
    EXPECT_EQ(reflect::typeOf<std::vector<f32>>().name, "array<f32>");
    EXPECT_EQ(reflect::typeOf<std::optional<std::string>>().name, "optional<string>");
    using UMap = std::unordered_map<std::string, i32>;
    EXPECT_EQ(reflect::typeOf<UMap>().kind, reflect::Kind::Map);
    EXPECT_TRUE(reflect::typeOf<std::vector<glm::vec3>>().packedLayout);
    EXPECT_FALSE(reflect::typeOf<std::vector<std::string>>().packedLayout);
}

TEST_F(Reflected, StructFieldsAndAttributes) {
    const auto& t = reflect::typeOf<Thing>();
    EXPECT_EQ(t.name, "Test.Thing");
    EXPECT_EQ(t.id, hashString("Test.Thing"));
    EXPECT_TRUE(t.registered);
    EXPECT_EQ(t.base, &reflect::typeOf<BaseThing>());
    ASSERT_EQ(t.fields.front().name, "label"); // inherited fields come first
    EXPECT_EQ(t.attributes.category, "Testing");
    const auto* count = t.findField("count");
    ASSERT_NE(count, nullptr);
    EXPECT_EQ(count->displayName(), "Count");
    EXPECT_EQ(count->attributes.tooltip, "How many");
    EXPECT_EQ(*count->attributes.step, 1.0);
    EXPECT_TRUE(count->attributes.saveGame);
    EXPECT_TRUE(t.findField("position")->attributes.replicated);
    const auto* hidden = t.findFieldOrFormer("secret");
    ASSERT_NE(hidden, nullptr);
    EXPECT_EQ(hidden->name, "hiddenValue");
    EXPECT_TRUE(hidden->attributes.hidden && hidden->attributes.readOnly && hidden->attributes.noSerialize);
    EXPECT_EQ(hidden->attributes.getMeta("icon"), "eye");
    EXPECT_EQ(*reflect::typeOf<Inner>().findField("weight")->attributes.rangeMax, 10.0);
    EXPECT_EQ(reflect::TypeRegistry::instance().find("Test.Thing"), &t);
    EXPECT_EQ(reflect::TypeRegistry::instance().findById(t.id), &t);
}

TEST_F(Reflected, ReRegistrationIsIdempotent) {
    const usize n = reflect::typeOf<Thing>().fields.size();
    registerTestTypes();
    EXPECT_EQ(reflect::typeOf<Thing>().fields.size(), n);
}

TEST_F(Reflected, EnumEntries) {
    const auto& t = reflect::typeOf<Shape>();
    EXPECT_EQ(t.kind, reflect::Kind::Enum);
    ASSERT_EQ(t.enumEntries.size(), 3u);
    EXPECT_EQ(t.findEnum("Capsule")->value, 2);
    EXPECT_EQ(t.findEnumByValue(1)->name, "Sphere");
    Shape s = Shape::Box;
    t.setEnum(&s, 2);
    EXPECT_EQ(s, Shape::Capsule);
}

TEST_F(Reflected, CreateAndDestroyThroughTypeInfo) {
    const auto& t = reflect::typeOf<Thing>();
    void* p = t.create();
    EXPECT_EQ(static_cast<Thing*>(p)->count, 3);
    Thing src;
    src.count = 42;
    t.copyAssign(p, &src);
    EXPECT_EQ(static_cast<Thing*>(p)->count, 42);
    t.destroy(p);
}

TEST_F(Reflected, ValueRefPaths) {
    Thing thing;
    thing.list.resize(2);
    thing.tags["a"] = 5;
    auto root = reflect::ValueRef::of(thing);

    auto x = reflect::resolvePath(root, "position.x");
    ASSERT_TRUE(x);
    EXPECT_FLOAT_EQ(*x.getAs<f32>(), 1.0f);
    EXPECT_TRUE(x.setAs(9.0));
    EXPECT_FLOAT_EQ(thing.position.x, 9.0f);

    EXPECT_TRUE(reflect::resolvePath(root, "inner.offset.z").setAs(4.0f));
    EXPECT_FLOAT_EQ(thing.inner.offset.z, 4.0f);
    EXPECT_TRUE(reflect::resolvePath(root, "list[1].weight").setAs(0.5f));
    EXPECT_FLOAT_EQ(thing.list[1].weight, 0.5f);
    EXPECT_TRUE(reflect::resolvePath(root, "list.0.weight").setAs(0.25f));
    EXPECT_FLOAT_EQ(thing.list[0].weight, 0.25f);
    EXPECT_EQ(*reflect::resolvePath(root, "tags[a]").getAs<i32>(), 5);
    EXPECT_EQ(*reflect::resolvePath(root, "tags.a").getAs<i64>(), 5);
    EXPECT_TRUE(reflect::resolvePath(root, "label").setAs(std::string("hello")));
    EXPECT_EQ(thing.label, "hello");
    EXPECT_TRUE(reflect::resolvePath(root, "rotation.w"));
    EXPECT_FALSE(reflect::resolvePath(root, "position.q"));
    EXPECT_FALSE(reflect::resolvePath(root, "list[7]"));
    EXPECT_FALSE(reflect::resolvePath(root, "nope"));
    EXPECT_FALSE(reflect::resolvePath(root, "maybe.value")); // empty optional

    // Enum set by name through the generic value path.
    EXPECT_TRUE(reflect::resolvePath(root, "shape").set(serial::Value::makeEnum("Capsule")));
    EXPECT_EQ(thing.shape, Shape::Capsule);
    EXPECT_TRUE(reflect::resolvePath(root, "shape").set(serial::Value::makeString("Box")));
    EXPECT_EQ(thing.shape, Shape::Box);
}

TEST_F(Reflected, CustomLeafHook) {
    struct Handle {
        Uuid id;
    };
    reflect::registerCustomLeaf<Handle>(
        "Test.Handle", serial::Tag::EntityRef, [](const Handle& h) { return serial::Value::makeEntityRef(h.id); },
        [](Handle& h, const serial::Value& v) {
            h.id = v.getUuid();
            return true;
        });
    Handle h{Uuid::generate()};
    const auto v = serial::toValue(h);
    EXPECT_EQ(v.tag(), serial::Tag::EntityRef);
    Handle back;
    EXPECT_TRUE(serial::fromValue(v, back));
    EXPECT_EQ(back.id, h.id);
}
