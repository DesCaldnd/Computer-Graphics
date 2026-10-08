// Built into ox_script_tests when the async module is configured (see engine/script/CMakeLists.txt).
#include "script_test_utils.hpp"

#include <oxwald/async/async.hpp>
#include <oxwald/script/async_bridge.hpp>

#include <thread>

using namespace ox;
using namespace ox::script;

namespace {

struct BridgeFixture : ::testing::Test {
    ScriptVM vm{ScriptVMConfig{.hotReloadInterval = 0.0}};
    AsyncBridge bridge{vm};
    sol::environment env; // created lazily: raw environments only see APIs bound before their creation

    void frame(f64 dt = 0.016) {
        bridge.update();
        vm.update(dt);
    }
    void run(std::string_view code) {
        if (!env.valid()) {
            env = vm.createEnvironment();
        }
        ScriptResult r = vm.runString(code, "test", &env);
        ASSERT_TRUE(r.ok) << r.error;
    }
};

} // namespace

TEST_F(BridgeFixture, LuaAwaitsCppFutureResolvedLater) {
    Promise<std::string> mesh;
    Promise<glm::vec3> spot;
    vm.bindApi("assets", [&](sol::state_view, sol::table& api) {
        api["load"] = [&](const std::string& path) { return bridge.wrap(mesh.future()); };
        api["spawnPoint"] = [&]() { return bridge.wrap(spot.future()); };
    });
    run(R"(
        steps = 0
        spawn(function()
            steps = 1
            local m = await(assets.load("props/door.glb"))
            steps = 2
            result = m
            local p = async.await(assets.spawnPoint())
            pos = p.y
        end)
    )");
    EXPECT_EQ(env["steps"].get<int>(), 1);
    for (int i = 0; i < 5; ++i) {
        frame();
    }
    EXPECT_EQ(env["steps"].get<int>(), 1); // parked, not polling

    std::thread([&] { mesh.setValue("MeshData"); }).join();
    EXPECT_EQ(bridge.pendingWakeups(), 1u);
    frame();
    EXPECT_EQ(env["steps"].get<int>(), 2);
    EXPECT_EQ(env["result"].get<std::string>(), "MeshData");

    spot.setValue(glm::vec3(1.f, 2.5f, 3.f));
    frame();
    EXPECT_FLOAT_EQ(env["pos"].get<float>(), 2.5f);
    EXPECT_EQ(vm.scheduler().taskCount(), 0u);
}

TEST_F(BridgeFixture, ReadyFuturesErrorsAndMisuse) {
    test::LogCapture logs;
    vm.bindApi("api", [&](sol::state_view, sol::table& t) {
        t["ready"] = [&]() { return bridge.wrap(makeReadyFuture(41)); };
        t["failing"] = [&]() { return bridge.wrap(makeErrorFuture<int>("asset not found")); };
        t["table"] = [&]() {
            sol::table tbl = vm.lua().create_table();
            tbl["hp"] = 7;
            return bridge.wrap(makeReadyFuture(sol::object(tbl)));
        };
        t["value"] = [&]() { return bridge.wrap(makeReadyFuture(ScriptValue{true})); };
    });
    run(R"(
        spawn(function()
            a = await(api.ready()) + 1
            local ok, err = pcall(function() return await(api.failing()) end)
            failedOk, failedMsg = ok, err
            hp = await(api.table()).hp
            flag = await(api.value())
            local f = api.ready()
            direct = f:isReady() and f:get()
        end)
        outside = pcall(function() return await(api.ready()) end)
        local okOut, errOut = pcall(await, api.ready())
        outsideMsg = errOut
    )");
    EXPECT_EQ(env["a"].get<int>(), 42);
    EXPECT_FALSE(env["failedOk"].get<bool>());
    EXPECT_NE(env["failedMsg"].get<std::string>().find("asset not found"), std::string::npos);
    EXPECT_EQ(env["hp"].get<int>(), 7);
    EXPECT_TRUE(env["flag"].get<bool>());
    EXPECT_EQ(env["direct"].get<int>(), 41);
    EXPECT_TRUE(env["outside"].get<bool>()); // already-ready futures work anywhere
    run(R"(
        local never = api.failing  -- keep api alive
        okPending, pendingMsg = pcall(function() return await(nil) end)
    )");
    EXPECT_FALSE(env["okPending"].get<bool>());
    EXPECT_NE(env["pendingMsg"].get<std::string>().find("expects a Future"), std::string::npos);
}

TEST_F(BridgeFixture, AwaitOutsideSpawnIsAnError) {
    Promise<int> p;
    vm.bindApi("api", [&](sol::state_view, sol::table& t) { t["pending"] = [&]() { return bridge.wrap(p.future()); }; });
    run(R"( ok, msg = pcall(function() return await(api.pending()) end) )");
    EXPECT_FALSE(env["ok"].get<bool>());
    EXPECT_NE(env["msg"].get<std::string>().find("spawn()"), std::string::npos);
}

TEST_F(BridgeFixture, CppAwaitsLuaFunctionResult) {
    run(R"(
        function compute(a, b)
            wait(0.25)
            return { sum = a + b, label = "ok" }
        end
        function broken() wait(0.1); error("script exploded") end
    )");
    CoroutineScheduler sched;
    int sum = 0;
    std::string label, failure;
    ScriptValue plain;
    sol::protected_function compute = env["compute"];
    sol::protected_function broken = env["broken"];
    auto h = sched.spawn([&]() -> Task<> {
        sol::main_object r = co_await bridge.call(compute, 2, 3);
        sol::table t = r.as<sol::table>();
        sum = t["sum"];
        label = t["label"];
        try {
            co_await bridge.call(broken);
        } catch (const AsyncError& e) {
            failure = e.what();
        }
        plain = co_await bridge.callValue(compute, 10, 5);
    });
    for (int i = 0; i < 60 && h.isRunning(); ++i) {
        frame(0.05);
        sched.tick(0.05);
    }
    EXPECT_EQ(h.status(), CoroutineStatus::Completed);
    EXPECT_EQ(sum, 5);
    EXPECT_EQ(label, "ok");
    EXPECT_NE(failure.find("script exploded"), std::string::npos);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(plain)); // tables are not plain values
}

TEST_F(BridgeFixture, CppAwaitsScriptInstanceMethod) {
    auto asset = vm.loadScriptFromString("door", R"(
        properties = { speed = { type = "float", default = 2 } }
        function openDoor(self, angle)
            self.opening = true
            wait(0.5)
            self.opening = false
            return angle * self.speed
        end
    )");
    auto inst = vm.createInstance(asset);
    ASSERT_TRUE(inst->create());
    CoroutineScheduler sched;
    double result = 0.0;
    std::string missing;
    sched.spawn([&]() -> Task<> {
        ScriptValue v = ScriptVM::fromLua(co_await bridge.invoke(*inst, "openDoor", 45.0));
        result = std::get<f64>(v);
        try {
            co_await bridge.invoke(*inst, "nope");
        } catch (const AsyncError& e) {
            missing = e.what();
        }
    });
    EXPECT_TRUE(inst->self()["opening"].get<bool>());
    for (int i = 0; i < 30 && result == 0.0; ++i) {
        frame(0.05);
        sched.tick(0.05);
    }
    EXPECT_DOUBLE_EQ(result, 90.0);
    EXPECT_NE(missing.find("nope"), std::string::npos);
}
