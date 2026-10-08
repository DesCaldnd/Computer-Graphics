#include "test_common.hpp"

#include "inspector/component_card.hpp"
#include "inspector/inspector_panel.hpp"
#include "inspector/property_editors.hpp"
#include "widgets/widgets.hpp"

#include <oxwald/core/reflect.hpp>
#include <oxwald/scene/component_registry.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>

namespace ox::editor::test {

enum class TestMode : u8 { Alpha, Beta, Gamma };

struct TestNested {
    f32 weight = 0.5f;
    std::string label = "nested";
};

// Every reflected field kind the inspector must handle.
struct TestAllTypes {
    bool flag = false;
    i32 count = 3;
    u32 mask = 7;
    f32 speed = 1.5f;
    f64 precise = 2.25;
    std::string title = "hello";
    glm::vec2 size{1.0f, 2.0f};
    glm::vec3 offset{0.0f};
    glm::vec4 rect{0.0f};
    glm::ivec3 cells{1, 2, 3};
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 tint{1.0f, 0.5f, 0.25f};
    glm::vec4 glow{2.0f, 1.0f, 0.5f, 1.0f};
    TestMode mode = TestMode::Alpha;
    Uuid texture;
    Uuid plainId = Uuid::fromName("plain");
    EntityRef target;
    std::vector<f32> weights{1.0f, 2.0f};
    std::vector<std::string> names{"a"};
    std::optional<f32> limit;
    std::map<std::string, i32> scores{{"alice", 1}};
    TestNested nested;
    glm::mat4 matrix{1.0f};
};

void registerTestTypes() {
    OX_REFLECT_ENUM(TestMode, "TestMode").value("Alpha", TestMode::Alpha).value("Beta", TestMode::Beta).value("Gamma", TestMode::Gamma);
    OX_REFLECT_TYPE(TestNested, "TestNested").field("weight", &TestNested::weight).field("label", &TestNested::label);
    OX_REFLECT_TYPE(TestAllTypes, "TestAllTypes")
        .attributes(attr::Category{"Testing"})
        .field("flag", &TestAllTypes::flag)
        .field("count", &TestAllTypes::count, attr::Range{0.0, 10.0})
        .field("mask", &TestAllTypes::mask)
        .field("speed", &TestAllTypes::speed, attr::Range{0.0, 100.0}, attr::Step{0.5})
        .field("precise", &TestAllTypes::precise)
        .field("title", &TestAllTypes::title)
        .field("size", &TestAllTypes::size)
        .field("offset", &TestAllTypes::offset)
        .field("rect", &TestAllTypes::rect)
        .field("cells", &TestAllTypes::cells)
        .field("orientation", &TestAllTypes::orientation)
        .field("tint", &TestAllTypes::tint, attr::Color{})
        .field("glow", &TestAllTypes::glow, attr::Color{true})
        .field("mode", &TestAllTypes::mode)
        .field("texture", &TestAllTypes::texture, attr::AssetRef{"Texture"})
        .field("plainId", &TestAllTypes::plainId)
        .field("target", &TestAllTypes::target)
        .field("weights", &TestAllTypes::weights)
        .field("names", &TestAllTypes::names)
        .field("limit", &TestAllTypes::limit)
        .field("scores", &TestAllTypes::scores)
        .field("nested", &TestAllTypes::nested)
        .field("matrix", &TestAllTypes::matrix, attr::ReadOnly{});
    ComponentRegistry::instance().add<TestAllTypes>({.icon = "cube"});
}

class InspectorTests : public QObject {
    Q_OBJECT
    std::unique_ptr<EditorContext> ctx;
    Uuid a, b;

    TestAllTypes& comp(const Uuid& id) { return ctx->editWorld().find(id).get<TestAllTypes>(); }

private Q_SLOTS:
    void initTestCase() { registerTestTypes(); }

    void init() {
        ctx = makeContext();
        Entity ea = ctx->editWorld().create("A");
        Entity eb = ctx->editWorld().create("B");
        ea.add<TestAllTypes>();
        eb.add<TestAllTypes>().speed = 9.0f;
        a = ea.uuid();
        b = eb.uuid();
    }
    void cleanup() { ctx.reset(); }

    void generatesEditorsForAllFieldTypes() {
        InspectorPanel panel(ctx.get());
        ctx->selection().select(a);
        panel.rebuild();
        ComponentCard* card = panel.card(QStringLiteral("TestAllTypes"));
        QVERIFY(card);
        for (const char* path : {"flag", "count", "mask", "speed", "precise", "title", "size", "offset", "rect", "cells", "orientation", "tint",
                                 "glow", "mode", "texture", "plainId", "target", "weights[0]", "weights[1]", "names[0]", "scores.alice",
                                 "nested.weight", "nested.label", "matrix"}) {
            QVERIFY2(card->editorForPath(path) != nullptr, path);
        }
        // optional without value has no element editor until set
        QVERIFY(card->editorForPath("limit.value") == nullptr);
        // editor types
        QVERIFY(card->editorForPath("flag")->findChild<QCheckBox*>());
        QVERIFY(card->editorForPath("mode")->findChild<QComboBox*>());
        QVERIFY(card->editorForPath("tint")->findChild<ColorButton*>());
        QVERIFY(card->editorForPath("glow")->findChildren<NumberField*>().size() == 1); // HDR intensity
        QCOMPARE(card->editorForPath("offset")->findChildren<NumberField*>().size(), 3);
        QCOMPARE(card->editorForPath("orientation")->findChildren<NumberField*>().size(), 3);
    }

    void writesBackThroughEditors() {
        InspectorPanel panel(ctx.get());
        ctx->selection().select(a);
        panel.rebuild();
        ComponentCard* card = panel.card(QStringLiteral("TestAllTypes"));
        QVERIFY(card);
        // bool
        card->editorForPath("flag")->findChild<QCheckBox*>()->click();
        QCOMPARE(comp(a).flag, true);
        // float via typed text
        auto* speed = card->editorForPath("speed")->findChild<NumberField*>();
        speed->lineEdit()->setText(QStringLiteral("42.5"));
        speed->lineEdit()->setModified(true);
        Q_EMIT speed->lineEdit()->editingFinished();
        QCOMPARE(comp(a).speed, 42.5f);
        // range clamp on int
        auto* count = card->editorForPath("count")->findChild<NumberField*>();
        count->lineEdit()->setText(QStringLiteral("99"));
        count->lineEdit()->setModified(true);
        Q_EMIT count->lineEdit()->editingFinished();
        QCOMPARE(comp(a).count, 10);
        // string
        auto* title = card->editorForPath("title")->findChild<QLineEdit*>();
        title->setText(QStringLiteral("world"));
        title->setModified(true);
        Q_EMIT title->editingFinished();
        QCOMPARE(comp(a).title, std::string("world"));
        // vector component
        auto fields = card->editorForPath("offset")->findChildren<NumberField*>();
        fields[1]->lineEdit()->setText(QStringLiteral("3"));
        fields[1]->lineEdit()->setModified(true);
        Q_EMIT fields[1]->lineEdit()->editingFinished();
        QCOMPARE(comp(a).offset.y, 3.0f);
        // quaternion through Euler degrees (yaw 90)
        auto rot = card->editorForPath("orientation")->findChildren<NumberField*>();
        rot[1]->lineEdit()->setText(QStringLiteral("90"));
        rot[1]->lineEdit()->setModified(true);
        Q_EMIT rot[1]->lineEdit()->editingFinished();
        const glm::vec3 e = quatToEulerDegrees(comp(a).orientation);
        QVERIFY(std::abs(e.y - 90.0f) < 0.01f);
        // enum
        auto* combo = card->editorForPath("mode")->findChild<QComboBox*>();
        combo->setCurrentIndex(2);
        Q_EMIT combo->activated(2);
        QCOMPARE(comp(a).mode, TestMode::Gamma);
        // nested struct
        auto* nested = card->editorForPath("nested.weight")->findChild<NumberField*>();
        nested->lineEdit()->setText(QStringLiteral("0.75"));
        nested->lineEdit()->setModified(true);
        Q_EMIT nested->lineEdit()->editingFinished();
        QCOMPARE(comp(a).nested.weight, 0.75f);
        // array element
        auto* w1 = card->editorForPath("weights[1]")->findChild<NumberField*>();
        w1->lineEdit()->setText(QStringLiteral("7"));
        w1->lineEdit()->setModified(true);
        Q_EMIT w1->lineEdit()->editingFinished();
        QCOMPARE(comp(a).weights[1], 7.0f);
        // container mutation through the context (array add, map erase)
        std::vector<serial::Value> arr{serial::toValue(std::vector<f32>{1, 2, 3})};
        ctx->setProperty({a}, "TestAllTypes", "weights", arr);
        QCOMPARE(comp(a).weights.size(), size_t(3));
        // every edit is undoable
        const int n = ctx->undoStack().count();
        QVERIFY(n >= 9);
        while (ctx->undoStack().canUndo()) ctx->undoStack().undo();
        QCOMPARE(comp(a).flag, false);
        QCOMPARE(comp(a).speed, 1.5f);
        QCOMPARE(comp(a).title, std::string("hello"));
        QCOMPARE(comp(a).weights.size(), size_t(2));
        QCOMPARE(comp(a).mode, TestMode::Alpha);
    }

    void multiSelectionShowsMixedValues() {
        InspectorPanel panel(ctx.get());
        ctx->selection().set({a, b});
        panel.rebuild();
        ComponentCard* card = panel.card(QStringLiteral("TestAllTypes"));
        QVERIFY(card);
        auto* speed = card->editorForPath("speed")->findChild<NumberField*>();
        QCOMPARE(speed->lineEdit()->property("mixed").toBool(), true);
        auto* precise = card->editorForPath("precise")->findChild<NumberField*>();
        QCOMPARE(precise->lineEdit()->property("mixed").toBool(), false);
        // editing a mixed field writes the same value to both entities
        speed->lineEdit()->setText(QStringLiteral("4"));
        speed->lineEdit()->setModified(true);
        Q_EMIT speed->lineEdit()->editingFinished();
        QCOMPARE(comp(a).speed, 4.0f);
        QCOMPARE(comp(b).speed, 4.0f);
        panel.refresh();
        QCOMPARE(speed->lineEdit()->property("mixed").toBool(), false);
        // a vector component edit keeps the other components of each entity
        comp(b).offset = {5, 6, 7};
        auto fields = card->editorForPath("offset")->findChildren<NumberField*>();
        fields[0]->lineEdit()->setText(QStringLiteral("1"));
        fields[0]->lineEdit()->setModified(true);
        Q_EMIT fields[0]->lineEdit()->editingFinished();
        QCOMPARE(comp(a).offset, glm::vec3(1, 0, 0));
        QCOMPARE(comp(b).offset, glm::vec3(1, 6, 7));
    }

    void addComponentPopupAddsComponent() {
        InspectorPanel panel(ctx.get());
        ctx->selection().select(a);
        panel.rebuild();
        QVERIFY(!ctx->editWorld().find(a).has<LightComponent>());
        QVERIFY(ctx->addComponent({a}, "Light"));
        pump();
        panel.rebuild();
        QVERIFY(panel.card(QStringLiteral("Light")));
        ctx->undoStack().undo();
        QVERIFY(!ctx->editWorld().find(a).has<LightComponent>());
    }
};

OX_EDITOR_TEST(InspectorTests);

} // namespace ox::editor::test

#include "test_inspector.moc"
