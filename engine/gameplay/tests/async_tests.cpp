#include "gameplay_test_utils.hpp"

#if defined(OX_GAMEPLAY_SCRIPT_ASYNC)

using namespace ox;
using namespace ox::gameplay;
using namespace ox::gameplay::test;

namespace {

void asyncServices(Services& s) {
    s.emplace<CoroutineScheduler>();
    auto& vm = s.emplace<script::ScriptVM>(testConfig().scriptVM);
    s.emplace<script::AsyncBridge>(vm);
}

Task<> ticker(int* count, bool* unwound) {
    struct Guard {
        bool* flag;
        ~Guard() { *flag = true; }
    } guard{unwound};
    for (;;) {
        co_await nextFrame();
        ++*count;
    }
}

} // namespace

TEST(GameplayAsync, LuaScriptAwaitsCppFutures) {
    GameplayHarness h(testConfig(), asyncServices);
    auto promise = std::make_shared<Promise<int>>();
    auto& bridge = h.services.get<script::AsyncBridge>();
    h.services.get<script::ScriptVM>().bindApi("testapi", [&](sol::state_view, sol::table& api) {
        api["value"] = [&bridge, promise] { return bridge.wrap(promise->future()); };
    });
    h.assets.addScript("waiter", R"(
        function onStart(self)
            spawn(function()
                self.waiting = true
                self.value = await(testapi.value())
                local hit = await(physics.raycastAsync(vec3(0, 5, 0), vec3(0, -1, 0), 20))
                self.hitName = hit and hit.entity and hit.entity.name
                await(scene.nextFrame())
                self.done = true
            end)
        end
    )");
    h.ground();
    Entity e = h.world.create("Waiter");
    e.add<ScriptComponent>().script = "waiter";
    h.start();
    h.run(0.2);
    sol::table self = h.runtime<ScriptRuntime>().self(e);
    ASSERT_TRUE(self.valid());
    EXPECT_TRUE(self["waiting"].get_or(false));
    EXPECT_FALSE(self["value"].valid() && self["value"].get_type() != sol::type::lua_nil) << "parked on the future";

    promise->setValue(42);
    h.run(0.2);
    EXPECT_EQ(self["value"].get_or(0), 42);
    EXPECT_EQ(self["hitName"].get_or(std::string()), "Ground");
    EXPECT_TRUE(self["done"].get_or(false));
}

TEST(GameplayAsync, ScriptCoroutineStartedFromCpp) {
    GameplayHarness h(testConfig(), asyncServices);
    h.assets.addScript("door", R"(
        function open(self, angle)
            wait(0.2)
            self.opened = true
            return 90
        end
    )");
    Entity door = h.world.create("Door");
    door.add<ScriptComponent>().script = "door";
    h.start();
    h.tick();
    Future<sol::main_object> f = h.runtime<ScriptRuntime>().startScriptCoroutine(door, "open");
    EXPECT_FALSE(f.isReady());
    h.run(0.5);
    ASSERT_TRUE(f.isReady());
    EXPECT_FALSE(f.hasError());
    EXPECT_EQ(f.get().as<int>(), 90);
}

TEST(GameplayAsync, CppCoroutineIsCancelledWhenItsEntityIsDestroyed) {
    GameplayHarness h(testConfig(), asyncServices);
    Entity bomb = h.world.create("Bomb");
    Entity other = h.world.create("Other");
    h.start();
    h.tick();
    int count = 0, otherCount = 0;
    bool unwound = false, otherUnwound = false;
    CoroutineHandle handle = startCoroutine(h.services, bomb, ticker(&count, &unwound), "Ticker");
    startCoroutine(h.services, other, ticker(&otherCount, &otherUnwound), "Other");
    ASSERT_TRUE(handle.valid());
    h.run(0.1);
    EXPECT_GE(count, 4);
    bomb.destroy();
    EXPECT_TRUE(unwound) << "cancelled (and unwound) as soon as the entity was scheduled for destruction";
    const int at = count;
    h.run(0.1);
    EXPECT_EQ(count, at);
    EXPECT_TRUE(handle.isDone());
    EXPECT_FALSE(h.world.valid(bomb.handle()));
    EXPECT_FALSE(otherUnwound);
    EXPECT_GT(otherCount, at);

    // Leaving play mode cancels the remaining entity coroutines.
    h.scheduler.setPlaying(false);
    h.tick();
    EXPECT_TRUE(otherUnwound);
}

#endif
