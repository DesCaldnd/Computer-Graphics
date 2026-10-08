#include <oxwald/ai/behavior_tree.hpp>
#include <oxwald/ai/utility_ai.hpp>

#include <gtest/gtest.h>

#include <filesystem>

using namespace ox;
using namespace ox::ai;

namespace {

// Action that runs for `ticks` ticks, then returns `result`. Logs enter/abort.
struct Scripted {
    std::vector<std::string>* log;
    std::string name;
    int ticks = 0;
    BTStatus result = BTStatus::Success;
    int counter = 0;

    BTAction::Fn fn() {
        return [this](BTContext&) {
            if (counter == 0) {
                log->push_back(name);
            }
            if (++counter > ticks) {
                counter = 0;
                return result;
            }
            return BTStatus::Running;
        };
    }
    BTAction::AbortFn onAbort() {
        return [this](BTContext&) {
            log->push_back(name + ":abort");
            counter = 0;
        };
    }
};

BTAction::Fn constant(std::vector<std::string>& log, std::string name, BTStatus s) {
    return [&log, name, s](BTContext&) {
        log.push_back(name);
        return s;
    };
}

} // namespace

TEST(Blackboard, TypedValuesAndObservers) {
    Blackboard bb;
    bb.set("health", 100);
    bb.set("speed", 2.5f);
    bb.set("name", "grunt");
    bb.set("target", glm::vec3(1, 2, 3));
    bb.set("alert", true);
    bb.set("entity", u64{42});
    EXPECT_EQ(bb.get<i32>("health"), 100);
    EXPECT_FLOAT_EQ(*bb.get<f32>("speed"), 2.5f);
    EXPECT_EQ(bb.get<std::string>("name"), "grunt");
    EXPECT_EQ(bb.get<glm::vec3>("target"), glm::vec3(1, 2, 3));
    EXPECT_EQ(bb.get<bool>("alert"), true);
    EXPECT_EQ(bb.get<u64>("entity"), 42u);
    EXPECT_FALSE(bb.get<f32>("health").has_value()) << "type mismatch yields nullopt";
    EXPECT_EQ(bb.getOr<i32>("missing", 7), 7);

    int healthCalls = 0, allCalls = 0;
    BlackboardValue lastOld, lastNew;
    const auto id = bb.observe("health", [&](const std::string&, const BlackboardValue& o, const BlackboardValue& n) {
        ++healthCalls;
        lastOld = o;
        lastNew = n;
    });
    bb.observe("", [&](const std::string&, const BlackboardValue&, const BlackboardValue&) { ++allCalls; });
    const u64 v0 = bb.version();
    bb.set("health", 100); // unchanged → no notification
    EXPECT_EQ(healthCalls, 0);
    EXPECT_EQ(bb.version(), v0);
    bb.set("health", 80);
    EXPECT_EQ(healthCalls, 1);
    EXPECT_EQ(std::get<i32>(lastOld), 100);
    EXPECT_EQ(std::get<i32>(lastNew), 80);
    bb.erase("health");
    EXPECT_EQ(healthCalls, 2);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(lastNew));
    bb.set("speed", 3.f);
    EXPECT_EQ(healthCalls, 2);
    EXPECT_EQ(allCalls, 3);
    bb.unobserve(id);
    bb.set("health", 1);
    EXPECT_EQ(healthCalls, 2);

    nlohmann::json j;
    bb.saveJson(j);
    Blackboard copy;
    copy.loadJson(j);
    EXPECT_EQ(copy.get<glm::vec3>("target"), glm::vec3(1, 2, 3));
    EXPECT_EQ(copy.get<u64>("entity"), 42u);
    EXPECT_EQ(copy.get<std::string>("name"), "grunt");
}

TEST(BehaviorTree, SequenceRunsInOrderAndStopsOnFailure) {
    std::vector<std::string> log;
    Scripted b{&log, "b", 2};
    auto tree = BTBuilder()
                    .sequence()
                    .action("a", constant(log, "a", BTStatus::Success))
                    .action("b", b.fn())
                    .action("c", constant(log, "c", BTStatus::Success))
                    .end()
                    .build();
    EXPECT_EQ(tree->tick(0.1f), BTStatus::Running);
    EXPECT_EQ(tree->tick(0.1f), BTStatus::Running);
    EXPECT_EQ(tree->tick(0.1f), BTStatus::Success);
    EXPECT_EQ(log, (std::vector<std::string>{"a", "b", "c"}));

    log.clear();
    auto failing = BTBuilder()
                       .sequence()
                       .action("a", constant(log, "a", BTStatus::Failure))
                       .action("c", constant(log, "c", BTStatus::Success))
                       .end()
                       .build();
    EXPECT_EQ(failing->tick(0.1f), BTStatus::Failure);
    EXPECT_EQ(log, (std::vector<std::string>{"a"}));
}

TEST(BehaviorTree, SelectorFallsBackAndStopsOnSuccess) {
    std::vector<std::string> log;
    auto tree = BTBuilder()
                    .selector()
                    .action("a", constant(log, "a", BTStatus::Failure))
                    .action("b", constant(log, "b", BTStatus::Success))
                    .action("c", constant(log, "c", BTStatus::Success))
                    .end()
                    .build();
    EXPECT_EQ(tree->tick(0.1f), BTStatus::Success);
    EXPECT_EQ(log, (std::vector<std::string>{"a", "b"}));

    auto none = BTBuilder().selector().condition("no", [](BTContext&) { return false; }).end().build();
    EXPECT_EQ(none->tick(0.1f), BTStatus::Failure);
}

TEST(BehaviorTree, ParallelPolicies) {
    std::vector<std::string> log;
    Scripted slow{&log, "slow", 3}, fast{&log, "fast", 1};
    auto all = BTBuilder()
                   .parallel(BTParallel::Policy::RequireAll, BTParallel::Policy::RequireOne)
                   .action("slow", slow.fn())
                   .action("fast", fast.fn())
                   .end()
                   .build();
    EXPECT_EQ(all->tick(0.1f), BTStatus::Running);
    EXPECT_EQ(all->tick(0.1f), BTStatus::Running); // fast done, slow still running
    EXPECT_EQ(all->tick(0.1f), BTStatus::Running);
    EXPECT_EQ(all->tick(0.1f), BTStatus::Success);

    Scripted slow2{&log, "slow2", 5}, fast2{&log, "fast2", 1};
    log.clear();
    auto one = BTBuilder()
                   .parallel(BTParallel::Policy::RequireOne, BTParallel::Policy::RequireOne)
                   .action("slow2", slow2.fn(), slow2.onAbort())
                   .action("fast2", fast2.fn())
                   .end()
                   .build();
    EXPECT_EQ(one->tick(0.1f), BTStatus::Running);
    EXPECT_EQ(one->tick(0.1f), BTStatus::Success);
    EXPECT_NE(std::find(log.begin(), log.end(), "slow2:abort"), log.end()) << "remaining children are aborted";

    auto failFast = BTBuilder()
                        .parallel()
                        .action("ok", [](BTContext&) { return BTStatus::Running; })
                        .action("bad", [](BTContext&) { return BTStatus::Failure; })
                        .end()
                        .build();
    EXPECT_EQ(failFast->tick(0.1f), BTStatus::Failure);
}

TEST(BehaviorTree, Decorators) {
    std::vector<std::string> log;
    auto inv = BTBuilder().inverter().action("f", constant(log, "f", BTStatus::Failure)).build();
    EXPECT_EQ(inv->tick(0.1f), BTStatus::Success);
    auto succ = BTBuilder().succeeder().action("f", constant(log, "f", BTStatus::Failure)).build();
    EXPECT_EQ(succ->tick(0.1f), BTStatus::Success);

    int count = 0;
    auto rep = BTBuilder()
                   .repeater(3)
                   .action("inc", [&](BTContext&) {
                       ++count;
                       return BTStatus::Success;
                   })
                   .build();
    EXPECT_EQ(rep->tick(0.1f), BTStatus::Running);
    EXPECT_EQ(rep->tick(0.1f), BTStatus::Running);
    EXPECT_EQ(rep->tick(0.1f), BTStatus::Success);
    EXPECT_EQ(count, 3);

    int fired = 0;
    auto cd = BTBuilder()
                  .cooldown(1.f)
                  .action("fire", [&](BTContext&) {
                      ++fired;
                      return BTStatus::Success;
                  })
                  .build();
    EXPECT_EQ(cd->tick(0.1f), BTStatus::Success);
    EXPECT_EQ(cd->tick(0.5f), BTStatus::Failure);
    EXPECT_EQ(cd->tick(0.3f), BTStatus::Failure);
    EXPECT_EQ(cd->tick(0.3f), BTStatus::Success);
    EXPECT_EQ(fired, 2);

    Scripted forever{&log, "forever", 1000};
    log.clear();
    auto to = BTBuilder().timeout(0.5f).action("forever", forever.fn(), forever.onAbort()).build();
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(to->tick(0.1f), BTStatus::Running);
    }
    EXPECT_EQ(to->tick(0.1f), BTStatus::Failure);
    EXPECT_EQ(log.back(), "forever:abort");

    auto wait = BTBuilder().sequence().wait(0.3f).setBlackboard("done", true).end().build();
    EXPECT_EQ(wait->tick(0.1f), BTStatus::Running);
    EXPECT_EQ(wait->tick(0.1f), BTStatus::Running);
    EXPECT_EQ(wait->tick(0.1f), BTStatus::Success);
    EXPECT_EQ(wait->blackboard().get<bool>("done"), true);
}

TEST(BehaviorTree, ObserverAbortsLowerPriorityAndSelf) {
    std::vector<std::string> log;
    Scripted attack{&log, "attack", 1000}, patrol{&log, "patrol", 1000};
    auto tree = BTBuilder()
                    .selector("Root")
                    .blackboardCondition("enemy", BBOp::IsSet, {}, BTAbortMode::Both, "HasEnemy")
                    .action("attack", attack.fn(), attack.onAbort())
                    .action("patrol", patrol.fn(), patrol.onAbort())
                    .end()
                    .build();
    EXPECT_EQ(tree->tick(0.1f), BTStatus::Running);
    EXPECT_EQ(log, (std::vector<std::string>{"patrol"}));
    tree->tick(0.1f);
    EXPECT_EQ(log.size(), 1u) << "patrol keeps running";

    tree->blackboard().set("enemy", u64{7});
    tree->tick(0.1f);
    EXPECT_EQ(log, (std::vector<std::string>{"patrol", "patrol:abort", "attack"}));
    EXPECT_TRUE(tree->findNode("attack")->running());

    // Unrelated key change does not disturb attack.
    tree->blackboard().set("noise", 1);
    tree->tick(0.1f);
    EXPECT_EQ(log.size(), 3u);

    tree->blackboard().erase("enemy");
    tree->tick(0.1f);
    EXPECT_EQ(log, (std::vector<std::string>{"patrol", "patrol:abort", "attack", "attack:abort", "patrol"}));
}

TEST(BehaviorTree, NoAbortWithoutObserverMode) {
    std::vector<std::string> log;
    Scripted attack{&log, "attack", 1000}, patrol{&log, "patrol", 1000};
    auto tree = BTBuilder()
                    .selector()
                    .blackboardCondition("enemy", BBOp::IsSet)
                    .action("attack", attack.fn(), attack.onAbort())
                    .action("patrol", patrol.fn(), patrol.onAbort())
                    .end()
                    .build();
    tree->tick(0.1f);
    tree->blackboard().set("enemy", true);
    tree->tick(0.1f);
    EXPECT_EQ(log, (std::vector<std::string>{"patrol"}));
}

TEST(BehaviorTree, ComparisonConditionsAndTrace) {
    auto tree = BTBuilder()
                    .selector("Root")
                    .sequence("LowHealth")
                    .blackboardCheck("health", BBOp::Less, 30)
                    .setBlackboard("state", "flee")
                    .end()
                    .setBlackboard("state", "fight")
                    .end()
                    .build();
    tree->blackboard().set("health", 80);
    EXPECT_EQ(tree->tick(0.1f), BTStatus::Success);
    EXPECT_EQ(tree->blackboard().get<std::string>("state"), "fight");
    tree->blackboard().set("health", 10.f); // float compared numerically against int
    tree->tick(0.1f);
    EXPECT_EQ(tree->blackboard().get<std::string>("state"), "flee");

    const auto trace = tree->trace();
    ASSERT_EQ(trace.size(), 5u);
    EXPECT_EQ(trace[0].name, "Root");
    EXPECT_EQ(trace[0].depth, 0u);
    EXPECT_EQ(trace[0].status, BTStatus::Success);
    EXPECT_EQ(trace[1].name, "LowHealth");
    EXPECT_EQ(trace[1].parentId, trace[0].id);
    EXPECT_EQ(trace[1].status, BTStatus::Success);
    EXPECT_TRUE(trace[1].tickedThisFrame);
    EXPECT_FALSE(trace[4].tickedThisFrame) << "fallback branch not evaluated this tick";

    int traced = 0;
    tree->setTraceCallback([&](const BTNode&, BTStatus) { ++traced; });
    tree->tick(0.1f);
    EXPECT_EQ(traced, 4);
}

TEST(BehaviorTree, JsonLoadSaveWithFactories) {
    BTFactory factory;
    int attacks = 0;
    factory.registerAction("Attack", [&](BTContext&) {
        ++attacks;
        return BTStatus::Success;
    });
    factory.registerCondition("IsAngry", [](BTContext& ctx) { return ctx.blackboard.getOr<bool>("angry", false); });
    factory.registerTree("Idle", nlohmann::json::parse(R"({
        "type": "Sequence", "name": "IdleSeq",
        "children": [ { "type": "Wait", "seconds": 0.2 }, { "type": "SetBlackboard", "key": "idled", "value": true } ]
    })"));

    const auto json = nlohmann::json::parse(R"({
        "blackboard": { "angry": false },
        "root": {
            "type": "Selector", "name": "Root",
            "children": [
                { "type": "BlackboardCondition", "key": "angry", "op": "Equals", "value": true, "abort": "LowerPriority",
                  "child": { "type": "Sequence", "children": [
                      { "type": "Condition", "condition": "IsAngry" },
                      { "type": "Action", "action": "Attack" } ] } },
                { "type": "Repeater", "count": -1, "child": { "type": "SubTree", "tree": "Idle" } }
            ]
        }
    })");
    auto tree = factory.load(json);
    ASSERT_NE(tree, nullptr);
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(tree->tick(0.1f), BTStatus::Running);
    }
    EXPECT_EQ(tree->blackboard().get<bool>("idled"), true);
    EXPECT_EQ(attacks, 0);
    tree->blackboard().set("angry", true);
    EXPECT_EQ(tree->tick(0.1f), BTStatus::Success);
    EXPECT_EQ(attacks, 1);

    // Save → load round trip produces the same JSON.
    const nlohmann::json saved = tree->toJson();
    auto reloaded = factory.load(saved);
    ASSERT_NE(reloaded, nullptr);
    EXPECT_EQ(reloaded->toJson(), saved);
    EXPECT_EQ(saved["children"][0]["abort"], "LowerPriority");
    EXPECT_EQ(saved["children"][1]["child"]["tree"], "Idle");

    const auto path = std::filesystem::temp_directory_path() / "ox_bt_test.json";
    ASSERT_TRUE(BTFactory::saveFile(*tree->root(), path));
    EXPECT_NE(factory.loadFile(path), nullptr);
    std::filesystem::remove(path);

    // Errors are reported, not thrown.
    EXPECT_EQ(factory.load(nlohmann::json::parse(R"({"type": "Nope"})")), nullptr);
    EXPECT_EQ(factory.load(nlohmann::json::parse(R"({"type": "Action", "action": "Unregistered"})")), nullptr);
}

TEST(UtilityAI, PicksHighestScoringAction) {
    UtilityScorer scorer;
    scorer.addAction({"Heal", {{"lowHealth", [](const Blackboard& bb) { return 1.f - bb.getOr<f32>("health", 1.f); }, {}}}});
    scorer.addAction({"Attack", {{"health", [](const Blackboard& bb) { return bb.getOr<f32>("health", 1.f); }, {}},
                                 {"enemyNear", [](const Blackboard& bb) { return bb.getOr<f32>("enemy", 0.f); }, {}}}});
    Blackboard bb;
    bb.set("health", 0.9f);
    bb.set("enemy", 1.f);
    EXPECT_EQ(scorer.best(bb), 1u);
    bb.set("health", 0.2f);
    EXPECT_EQ(scorer.best(bb), 0u);
    bb.set("health", 1.f);
    bb.set("enemy", 0.f);
    EXPECT_FALSE(scorer.best(bb).has_value());

    ResponseCurve logistic{ResponseCurve::Type::Logistic, 1.f, 1.f, 0.f, 0.f};
    EXPECT_NEAR(logistic.evaluate(0.5f), 0.5f, 1e-4f);
    EXPECT_GT(logistic.evaluate(0.9f), 0.95f);
}
