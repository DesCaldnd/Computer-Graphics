// Guide chapter 13 «ИИ»: blackboard, behaviour trees (builder + JSON), observer aborts, trace.
#include <oxwald/ai/behavior_tree.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

using namespace ox;
using namespace ox::ai;

TEST(GuideAiBlackboard, TypedValuesAndObservers) {
    Blackboard bb;
    bb.set("health", 100);              // → i32
    bb.set("speed", 2.5f);              // → f32
    bb.set("state", "patrol");          // → std::string
    bb.set("home", glm::vec3(1, 0, 3)); // → glm::vec3
    bb.set("target", u64{42});          // беззнаковое → u64 (id сущности)

    EXPECT_EQ(bb.get<i32>("health"), 100);
    EXPECT_FALSE(bb.get<f32>("health")) << "тип не совпал → nullopt, без неявных конверсий";
    EXPECT_EQ(bb.getOr<i32>("ammo", 0), 0);

    int changes = 0;
    const Blackboard::ObserverId id =
        bb.observe("health", [&](const std::string&, const BlackboardValue& oldV, const BlackboardValue& newV) {
            ++changes;
            EXPECT_EQ(std::get<i32>(oldV), 100);
            EXPECT_EQ(std::get<i32>(newV), 80);
        });
    bb.set("health", 100); // то же значение — наблюдатели молчат
    bb.set("health", 80);
    EXPECT_EQ(changes, 1);
    bb.unobserve(id);

    nlohmann::json j; // сохранение в сейв/отладочный дамп
    bb.saveJson(j);
    Blackboard copy;
    copy.loadJson(j);
    EXPECT_EQ(copy.get<glm::vec3>("home"), glm::vec3(1, 0, 3));
    EXPECT_EQ(copy.get<u64>("target"), 42u);
}

namespace {

// Контроллер «охранника» — то, что в игре делает реальную работу (движение, стрельба).
struct Guard {
    int shots = 0;
    int patrolSteps = 0;
    bool moving = false;
};

} // namespace

TEST(GuideAiBehaviorTree, GuardPatrolsAndSwitchesToCombat) {
    Guard guard;
    auto blackboard = std::make_shared<Blackboard>();

    auto tree = BTBuilder()
                    .selector("Root")
                        // Декоратор с наблюдателем: как только появится target — прервать патруль (LowerPriority),
                        // как только target пропадёт — прервать бой (Self).
                        .blackboardCondition("target", BBOp::IsSet, {}, BTAbortMode::Both, "HasTarget")
                            .sequence("Combat")
                                .action("Shoot", [](BTContext& ctx) {
                                    static_cast<Guard*>(ctx.user)->shots++;
                                    return BTStatus::Success;
                                })
                                .wait(0.5f)
                            .end()
                        .sequence("Patrol")
                            .action("MoveToWaypoint",
                                    [](BTContext& ctx) {
                                        auto* g = static_cast<Guard*>(ctx.user);
                                        g->moving = ++g->patrolSteps % 3 != 0; // дошли до точки за 3 тика
                                        return g->moving ? BTStatus::Running : BTStatus::Success;
                                    },
                                    [](BTContext& ctx) { static_cast<Guard*>(ctx.user)->moving = false; }) // onAbort
                            .wait(0.2f)
                        .end()
                    .end()
                    .build(blackboard);
    tree->setUserData(&guard); // доступно в узлах как ctx.user

    for (int i = 0; i < 5; ++i) {
        tree->tick(0.1f);
    }
    EXPECT_EQ(guard.shots, 0);
    EXPECT_TRUE(guard.moving) << "идёт к следующей точке патруля";

    // Восприятие заметило игрока — пишем в blackboard; на следующем тике патруль будет прерван.
    blackboard->set("target", u64{7});
    tree->tick(0.1f);
    EXPECT_EQ(guard.shots, 1);
    EXPECT_FALSE(guard.moving) << "onAbort остановил движение";
    EXPECT_TRUE(tree->findNode("Combat")->running());

    // Цель потеряна — бой прерывается, охранник возвращается к патрулю.
    blackboard->erase("target");
    tree->tick(0.1f);
    EXPECT_TRUE(tree->findNode("Patrol")->running());

    // Трассировка для отладчика/визуализатора дерева.
    const auto trace = tree->trace();
    ASSERT_FALSE(trace.empty());
    EXPECT_EQ(trace[0].name, "Root");
    EXPECT_EQ(trace[0].status, BTStatus::Running);
}

TEST(GuideAiBehaviorTree, LoadFromJsonFile) {
    BTFactory factory; // встроенные узлы уже зарегистрированы
    int shots = 0;
    factory.registerAction("Shoot", [&](BTContext& ctx) {
        ++shots;
        ctx.blackboard.set("ammo", ctx.blackboard.getOr<i32>("ammo", 0) - 1);
        return BTStatus::Success;
    });
    factory.registerAction("MoveToWaypoint", [](BTContext&) { return BTStatus::Success; });
    // Поддерево, на которое ссылается "SubTree": {"tree": "Patrol"}.
    factory.registerTree("Patrol", nlohmann::json::parse(R"({
        "type": "Sequence", "children": [
            { "type": "Action", "action": "MoveToWaypoint" },
            { "type": "Wait", "seconds": 1.0 } ] })"));

    const std::filesystem::path file = std::filesystem::path(OX_GUIDE_DIR) / "guard.bt.json";
    std::unique_ptr<BehaviorTree> tree = factory.loadFile(file);
    ASSERT_NE(tree, nullptr); // ошибки (неизвестный тип/действие) логируются, а не бросаются
    EXPECT_EQ(tree->blackboard().get<i32>("ammo"), 3); // начальные значения из секции "blackboard"

    tree->tick(0.1f);
    EXPECT_EQ(shots, 0) << "цели нет — патруль";

    tree->blackboard().set("target", u64{1});
    for (int i = 0; i < 20; ++i) {
        tree->tick(0.1f);
    }
    EXPECT_EQ(shots, 3) << "стреляет, пока ammo > 0";
    EXPECT_EQ(tree->blackboard().get<i32>("ammo"), 0);

    // Дерево можно сохранить обратно (редактор) и загрузить снова.
    const nlohmann::json saved = tree->toJson();
    auto reloaded = factory.load(saved);
    ASSERT_NE(reloaded, nullptr);
    EXPECT_EQ(reloaded->toJson(), saved);
}

namespace {

// Свой узел: успех, когда до точки из blackboard ближе radius.
class IsNear final : public BTNode {
public:
    IsNear(std::string key, f32 radius) : BTNode("IsNear"), m_key(std::move(key)), m_radius(radius) {}
    [[nodiscard]] const char* typeName() const override { return "IsNear"; }
    void saveParams(nlohmann::json& j) const override {
        j["key"] = m_key;
        j["radius"] = m_radius;
    }

protected:
    BTStatus onTick(BTContext& ctx) override {
        const glm::vec3 self = ctx.blackboard.getOr<glm::vec3>("position", glm::vec3(0.f));
        const auto goal = ctx.blackboard.get<glm::vec3>(m_key);
        return goal && glm::distance(self, *goal) < m_radius ? BTStatus::Success : BTStatus::Failure;
    }

private:
    std::string m_key;
    f32 m_radius;
};

} // namespace

TEST(GuideAiBehaviorTree, CustomNodeInBuilderAndFactory) {
    BTFactory factory;
    factory.registerNode("IsNear", [](const nlohmann::json& j, const BTFactory&) {
        return std::make_unique<IsNear>(j.value("key", std::string("goal")), j.value("radius", 1.f));
    });
    auto tree = factory.load(nlohmann::json::parse(R"({ "type": "IsNear", "key": "goal", "radius": 2.0 })"));
    ASSERT_NE(tree, nullptr);
    tree->blackboard().set("goal", glm::vec3(1, 0, 0));
    EXPECT_EQ(tree->tick(0.1f), BTStatus::Success);

    auto built = BTBuilder().inverter().node(std::make_unique<IsNear>("goal", 2.f)).build();
    built->blackboard().set("goal", glm::vec3(10, 0, 0));
    EXPECT_EQ(built->tick(0.1f), BTStatus::Success); // далеко → IsNear = Failure → Inverter = Success
}
