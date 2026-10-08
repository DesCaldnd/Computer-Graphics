#include <oxwald/scene/scene.hpp>
#include <oxwald/scene/system.hpp>

#include <gtest/gtest.h>

using namespace ox;

namespace {

struct Recorder {
    std::vector<std::string> calls;
};

class RecordingSystem : public ISystem {
public:
    RecordingSystem(std::string name, SystemPhase phase, i32 order, bool playOnly = false)
        : m_name(std::move(name)), m_phase(phase), m_order(order), m_playOnly(playOnly) {}
    std::string_view name() const override { return m_name; }
    SystemPhase phase() const override { return m_phase; }
    i32 order() const override { return m_order; }
    bool playModeOnly() const override { return m_playOnly; }
    void onAttach(World&, Services&) override { ++attached; }
    void onDetach(World&, Services&) override { ++detached; }
    void update(SystemContext& ctx) override {
        ctx.services.get<Recorder>().calls.push_back(m_name);
        lastDt = ctx.dt;
        lastAlpha = ctx.alpha;
        EXPECT_EQ(ctx.phase, m_phase);
    }
    int attached = 0;
    int detached = 0;
    f32 lastDt = 0;
    f32 lastAlpha = 0;

private:
    std::string m_name;
    SystemPhase m_phase;
    i32 m_order;
    bool m_playOnly;
};

struct SchedulerTest : ::testing::Test {
    void SetUp() override {
        registerSceneTypes();
        services.emplace<Recorder>();
    }
    std::vector<std::string>& calls() { return services.get<Recorder>().calls; }
    World world;
    Services services;
};

} // namespace

TEST_F(SchedulerTest, PhasesAndOrder) {
    SystemScheduler s(1.0 / 60.0);
    s.emplace<RecordingSystem>("extract", SystemPhase::Extract, 0);
    s.emplace<RecordingSystem>("update.late", SystemPhase::Update, 10);
    s.emplace<RecordingSystem>("update.a", SystemPhase::Update, 0);
    s.emplace<RecordingSystem>("update.b", SystemPhase::Update, 0);
    s.emplace<RecordingSystem>("pre", SystemPhase::PreUpdate, 0);
    s.emplace<RecordingSystem>("post", SystemPhase::PostUpdate, 0);
    s.emplace<RecordingSystem>("fixed", SystemPhase::FixedUpdate, 0);
    s.emplace<TransformSystem>();

    s.tick(world, services, 1.0 / 60.0);
    EXPECT_EQ(calls(), (std::vector<std::string>{"pre", "fixed", "update.a", "update.b", "update.late", "post", "extract"}));
    ASSERT_EQ(s.systems(SystemPhase::PostUpdate).size(), 2u);
    EXPECT_EQ(s.systems(SystemPhase::PostUpdate)[0]->name(), "Transform") << "transform system runs first in PostUpdate";
    EXPECT_EQ(s.frame(), 1u);
}

TEST_F(SchedulerTest, FixedTimestepStepsAndAlpha) {
    SystemScheduler s(0.01, 5);
    auto& fixed = s.emplace<RecordingSystem>("fixed", SystemPhase::FixedUpdate, 0);
    auto& update = s.emplace<RecordingSystem>("update", SystemPhase::Update, 0);

    s.tick(world, services, 0.035); // 3 steps, 0.005 left
    EXPECT_EQ(std::count(calls().begin(), calls().end(), "fixed"), 3);
    EXPECT_FLOAT_EQ(fixed.lastDt, 0.01f);
    EXPECT_FLOAT_EQ(update.lastDt, 0.035f);
    EXPECT_NEAR(update.lastAlpha, 0.5f, 1e-4f);

    calls().clear();
    s.tick(world, services, 0.004); // accumulates to 0.009: no step
    EXPECT_EQ(std::count(calls().begin(), calls().end(), "fixed"), 0);

    calls().clear();
    s.tick(world, services, 1.0); // spiral-of-death guard: at most 5 steps
    EXPECT_EQ(std::count(calls().begin(), calls().end(), "fixed"), 5);
    EXPECT_LT(s.fixedTimestep().accumulator(), 0.01);
}

TEST_F(SchedulerTest, EnableDisablePlayModeAndAttach) {
    SystemScheduler s;
    auto& game = s.emplace<RecordingSystem>("game", SystemPhase::Update, 0, true);
    auto& always = s.emplace<RecordingSystem>("always", SystemPhase::Update, 1);
    s.attach(world, services);
    EXPECT_EQ(game.attached, 1);
    auto& late = s.emplace<RecordingSystem>("late", SystemPhase::Update, 2);
    EXPECT_EQ(late.attached, 1) << "systems added after attach are attached immediately";

    s.tick(world, services, 0.016);
    EXPECT_EQ(calls(), (std::vector<std::string>{"always", "late"}));
    calls().clear();
    s.setPlaying(true);
    s.setEnabled("always", false);
    EXPECT_FALSE(s.isEnabled("always"));
    s.tick(world, services, 0.016);
    EXPECT_EQ(calls(), (std::vector<std::string>{"game", "late"}));
    EXPECT_TRUE(s.remove("late"));
    EXPECT_EQ(s.find("late"), nullptr);
    s.detach();
    EXPECT_EQ(game.detached, 1);
    EXPECT_EQ(always.detached, 1);
}

TEST_F(SchedulerTest, TickPropagatesTransformsAndFlushesDestroyed) {
    SystemScheduler s;
    s.emplace<TransformSystem>();
    Entity e = world.create("E");
    e.setPosition({1, 2, 3});
    Entity doomed = world.create("Doomed");
    doomed.destroy();
    s.tick(world, services, 0.016);
    EXPECT_EQ(glm::vec3(e.get<WorldTransformComponent>().matrix[3]), glm::vec3(1, 2, 3));
    EXPECT_FALSE(doomed.valid());
    e.setPosition({4, 5, 6});
    s.tick(world, services, 0.016);
    EXPECT_EQ(glm::vec3(e.get<WorldTransformComponent>().previous[3]), glm::vec3(1, 2, 3));
}
