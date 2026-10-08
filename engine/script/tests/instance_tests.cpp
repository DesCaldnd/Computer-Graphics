#include "script_test_utils.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>

using namespace ox;
using namespace ox::script;
using namespace ox::script::test;
namespace fs = std::filesystem;

namespace {

// Collects calls from scripts through a C++ binding (`probe.record("x")`).
struct Probe {
    std::vector<std::string> calls;
    void install(ScriptVM& vm) {
        vm.bindApi("probe", [this](sol::state_view, sol::table& api) {
            api["record"] = [this](const std::string& s) { calls.push_back(s); };
        });
    }
};

fs::path tempDir(std::string_view name) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = fs::temp_directory_path() / std::format("ox_script_{}_{}", name, stamp);
    fs::create_directories(dir);
    return dir;
}

void writeFile(const fs::path& path, std::string_view text, int mtimeBumpSeconds = 0) {
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
    }
    if (mtimeBumpSeconds != 0) {
        // Make the change visible even on filesystems with coarse timestamps.
        fs::last_write_time(path, fs::last_write_time(path) + std::chrono::seconds(mtimeBumpSeconds));
    }
}

} // namespace

TEST(ScriptProperties, DeclarationsAreParsedWithTypesDefaultsAndRanges) {
    ScriptVMConfig cfg;
    cfg.searchRoots = {OX_TEST_DATA_DIR};
    ScriptVM vm(cfg);
    auto asset = vm.loadScript(fs::path(OX_TEST_DATA_DIR) / "sample_player.lua");
    ASSERT_TRUE(asset->valid());
    const auto& props = asset->properties();
    ASSERT_EQ(props.size(), 5u);
    // order 0, order 1, then order-less (0)... sorted by (order, name)
    EXPECT_EQ(props[0].name, "godMode");
    EXPECT_EQ(props[0].type, ScriptPropertyType::Bool);

    const ScriptPropertyDesc* speed = asset->findProperty("speed");
    ASSERT_NE(speed, nullptr);
    EXPECT_EQ(speed->type, ScriptPropertyType::Float);
    EXPECT_EQ(std::get<f64>(speed->defaultValue), 5.0);
    EXPECT_EQ(speed->min, 0.0);
    EXPECT_EQ(speed->max, 20.0);
    EXPECT_EQ(speed->tooltip, "Units per second");

    const ScriptPropertyDesc* hp = asset->findProperty("maxHealth");
    ASSERT_NE(hp, nullptr);
    EXPECT_EQ(hp->type, ScriptPropertyType::Int);
    EXPECT_EQ(std::get<i64>(hp->defaultValue), 100);
    EXPECT_EQ(hp->order, 1);

    EXPECT_EQ(std::get<glm::vec4>(asset->findProperty("tint")->defaultValue), glm::vec4(1, 0.5f, 0, 1));
    EXPECT_EQ(asset->findProperty("tint")->type, ScriptPropertyType::Color);
    EXPECT_EQ(std::get<glm::vec3>(asset->findProperty("spawnAt")->defaultValue), glm::vec3(0, 1, 0));

    // Per-instance overrides are type-checked and clamped.
    auto inst = vm.createInstance(asset);
    EXPECT_TRUE(inst->setProperty("speed", 50.0));
    EXPECT_TRUE(inst->setProperty("maxHealth", 250.4));
    EXPECT_FALSE(inst->setProperty("speed", std::string("fast")));
    EXPECT_FALSE(inst->setProperty("nope", 1.0));
    ASSERT_TRUE(inst->create());
    EXPECT_EQ(std::get<f64>(inst->getProperty("speed")), 20.0);
    EXPECT_EQ(std::get<i64>(inst->getProperty("maxHealth")), 250);
    EXPECT_EQ(inst->self()["health"].get<i64>(), 250);
    EXPECT_EQ(inst->self()["spawnAt"].get<glm::vec3>(), glm::vec3(0, 1, 0));
}

TEST(ScriptInstance, LifecycleCallOrder) {
    ScriptVM vm;
    Probe probe;
    probe.install(vm);
    auto asset = vm.loadScriptFromString("order.lua", R"(
        function onCreate(self) probe.record("create:" .. tostring(self.injected)) end
        function onStart(self) probe.record("start") end
        function onUpdate(self, dt) probe.record("update:" .. dt) end
        function onFixedUpdate(self, dt) probe.record("fixed:" .. dt) end
        function onEvent(self, name, payload) probe.record("event:" .. name .. ":" .. tostring(payload)) end
        function onDestroy(self) probe.record("destroy") end
    )");
    {
        auto inst = vm.createInstance(asset, [](ScriptInstance&, sol::table& self) { self["injected"] = 42; });
        EXPECT_EQ(inst->state(), ScriptInstance::State::Uninitialized);
        inst->update(0.5f); // ignored before create
        ASSERT_TRUE(inst->create());
        inst->fixedUpdate(0.25f); // implicitly starts
        inst->update(0.5f);
        inst->sendEvent("hit", ScriptValue{3.0});
        EXPECT_EQ(inst->state(), ScriptInstance::State::Started);
    } // destructor destroys
    EXPECT_EQ(probe.calls, (std::vector<std::string>{"create:42", "start", "fixed:0.25", "update:0.5", "event:hit:3.0",
                                                     "destroy"}));
}

TEST(ScriptInstance, ErrorsInCallbacksDoNotStopTheInstance) {
    ScriptVM vm;
    LogCapture logs;
    auto asset = vm.loadScriptFromString("flaky.lua", "function onUpdate(self, dt)\n"
                                                       "  self.n = (self.n or 0) + 1\n"
                                                       "  if self.n == 2 then error('boom') end\n"
                                                       "end\n");
    auto inst = vm.createInstance(asset);
    ASSERT_TRUE(inst->create());
    for (int i = 0; i < 4; ++i) inst->update(0.1f);
    EXPECT_EQ(inst->errorCount(), 1u);
    EXPECT_EQ(inst->self()["n"].get<int>(), 4);
    EXPECT_TRUE(logs.contains("flaky.lua:3: boom"));
    EXPECT_TRUE(logs.contains("flaky.lua:onUpdate"));
}

TEST(ScriptInstance, InstancesHaveSeparateState) {
    ScriptVM vm;
    auto asset = vm.loadScriptFromString("counter.lua", "counter = 0\n"
                                                         "function onUpdate(self) counter = counter + 1; "
                                                         "self.mine = counter end");
    auto a = vm.createInstance(asset);
    auto b = vm.createInstance(asset);
    a->create();
    b->create();
    a->update(0);
    a->update(0);
    b->update(0);
    EXPECT_EQ(a->self()["mine"].get<int>(), 2);
    EXPECT_EQ(b->self()["mine"].get<int>(), 1);
}

TEST(ScriptHotReload, PreservesStateAndCallsOnReload) {
    const fs::path dir = tempDir("reload");
    const fs::path file = dir / "mover.lua";
    writeFile(file, R"(
        properties = { step = { type = "float", default = 1 } }
        function onCreate(self) self.count = 0 end
        function onUpdate(self, dt) self.count = self.count + self.step end
    )");
    ScriptVMConfig cfg;
    cfg.hotReloadInterval = 0; // poll manually
    ScriptVM vm(cfg);
    LogCapture logs;
    auto asset = vm.loadScript(file);
    auto inst = vm.createInstance(asset);
    ASSERT_TRUE(inst->create());
    inst->update(0.f);
    inst->update(0.f);
    EXPECT_EQ(inst->self()["count"].get<f64>(), 2.0);
    EXPECT_EQ(vm.pollHotReload(), 0u);

    writeFile(file, R"(
        properties = { step = { type = "float", default = 1 }, bonus = { type = "int", default = 100 } }
        function onCreate(self) self.count = 0 end
        function onUpdate(self, dt) self.count = self.count + self.step * 10 end
        function on_reload(self) self.reloads = (self.reloads or 0) + 1 end
    )",
              2);
    EXPECT_EQ(vm.pollHotReload(), 1u);
    EXPECT_EQ(asset->version(), 2u);
    EXPECT_EQ(inst->loadedVersion(), 2u);
    EXPECT_EQ(inst->self()["count"].get<f64>(), 2.0);  // state kept
    EXPECT_EQ(inst->self()["reloads"].get<int>(), 1);  // on_reload called
    EXPECT_EQ(inst->self()["bonus"].get<i64>(), 100);  // new property gets its default
    EXPECT_NE(asset->findProperty("bonus"), nullptr);  // editor sees the new declaration
    inst->update(0.f);
    EXPECT_EQ(inst->self()["count"].get<f64>(), 12.0); // new code runs

    // A broken edit keeps the previous version running.
    writeFile(file, "function onUpdate(self) self.count = = 1 end", 4);
    EXPECT_EQ(vm.pollHotReload(), 0u);
    EXPECT_TRUE(logs.contains("keeping version 2"));
    inst->update(0.f);
    EXPECT_EQ(inst->self()["count"].get<f64>(), 22.0);

    inst.reset();
    asset.reset();
    fs::remove_all(dir);
}

TEST(ScriptHotReload, ModuleChangeReloadsDependents) {
    const fs::path dir = tempDir("modules");
    writeFile(dir / "cfg.lua", "return { value = 1 }");
    writeFile(dir / "user.lua", "local cfg = require('cfg')\nfunction onUpdate(self) self.v = cfg.value end");
    ScriptVMConfig cfg;
    cfg.searchRoots = {dir};
    cfg.hotReloadInterval = 0;
    ScriptVM vm(cfg);
    auto inst = vm.createInstance(vm.loadScript(dir / "user.lua"));
    ASSERT_TRUE(inst->create());
    inst->update(0);
    EXPECT_EQ(inst->self()["v"].get<int>(), 1);
    writeFile(dir / "cfg.lua", "return { value = 2 }", 2);
    EXPECT_EQ(vm.pollHotReload(), 1u);
    inst->update(0);
    EXPECT_EQ(inst->self()["v"].get<int>(), 2);
    inst.reset();
    fs::remove_all(dir);
}

TEST(ScriptScheduler, WaitUsesScriptTime) {
    ScriptVM vm;
    auto asset = vm.loadScriptFromString("waiter.lua", R"(
        function onStart(self)
            self.log = {}
            spawn(function(tag)
                table.insert(self.log, tag .. "@" .. time())
                wait(1.0)
                table.insert(self.log, "b@" .. time())
                wait(0.5)
                table.insert(self.log, "c@" .. time())
                wait()  -- next frame
                table.insert(self.log, "d@" .. time())
            end, "a")
            self.badWait = not pcall(wait, 1) -- wait outside spawn is an error
        end
    )");
    auto inst = vm.createInstance(asset);
    ASSERT_TRUE(inst->create());
    inst->start();
    auto log = [&] {
        std::vector<std::string> out;
        sol::table t = inst->self()["log"];
        for (usize i = 1; i <= t.size(); ++i) out.push_back(t[i].get<std::string>());
        return out;
    };
    EXPECT_EQ(log(), std::vector<std::string>{"a@0.0"});
    EXPECT_TRUE(inst->self()["badWait"].get<bool>());
    for (int i = 0; i < 3; ++i) vm.update(0.25); // t = 0.75
    EXPECT_EQ(log().size(), 1u);
    vm.update(0.25); // t = 1.0
    EXPECT_EQ(log().back(), "b@1.0");
    vm.update(0.25);
    EXPECT_EQ(log().size(), 2u);
    vm.update(0.25); // t = 1.5
    EXPECT_EQ(log().back(), "c@1.5");
    vm.update(0.25);
    EXPECT_EQ(log().back(), "d@1.75");
    EXPECT_EQ(vm.scheduler().taskCount(), 0u);
}

TEST(ScriptScheduler, TimersAndCleanupOnDestroy) {
    ScriptVM vm;
    auto asset = vm.loadScriptFromString("timers.lua", R"(
        function onCreate(self)
            self.once, self.ticks = 0, 0
            timer.after(0.5, function() self.once = self.once + 1 end)
            self.every = timer.every(0.25, function() self.ticks = self.ticks + 1 end)
            spawn(function() while true do wait(0.1) end end)
        end
        function onEvent(self, name) if name == "stop" then timer.cancel(self.every) end end
    )");
    auto inst = vm.createInstance(asset);
    ASSERT_TRUE(inst->create());
    for (int i = 0; i < 8; ++i) vm.update(0.125); // t = 1.0 (binary-exact steps)
    EXPECT_EQ(inst->self()["once"].get<int>(), 1);
    EXPECT_EQ(inst->self()["ticks"].get<int>(), 4);
    inst->sendEvent("stop");
    for (int i = 0; i < 8; ++i) vm.update(0.125);
    EXPECT_EQ(inst->self()["once"].get<int>(), 1);
    EXPECT_EQ(inst->self()["ticks"].get<int>(), 4);
    EXPECT_EQ(vm.scheduler().taskCount(), 1u);
    inst->destroy();
    EXPECT_EQ(vm.scheduler().taskCount(), 0u); // coroutine of the destroyed instance is gone
    EXPECT_EQ(vm.scheduler().timerCount(), 0u);
}

TEST(ScriptEvents, BridgeBetweenLuaAndCpp) {
    ScriptVMConfig cfg;
    cfg.searchRoots = {OX_TEST_DATA_DIR};
    ScriptVM vm(cfg);
    auto inst = vm.createInstance(vm.loadScript(fs::path(OX_TEST_DATA_DIR) / "sample_player.lua"));
    ASSERT_TRUE(inst->create());
    EXPECT_EQ(vm.events().subscriberCount("player.damage"), 1u);

    // C++ -> Lua: table payload built on the C++ side.
    sol::table payload = vm.lua().create_table();
    payload["amount"] = 30;
    vm.events().publish("player.damage", payload);
    EXPECT_EQ(inst->self()["health"].get<int>(), 70);

    // Direct instance event with a plain value.
    inst->sendEvent("heal", ScriptValue{i64{20}});
    EXPECT_EQ(inst->self()["health"].get<int>(), 90);

    // Lua -> C++
    i64 diedTicks = -1;
    vm.events().subscribe("player.died", [&](std::string_view, const sol::object& p) {
        diedTicks = p.as<sol::table>()["ticks"].get<i64>();
    });
    for (int i = 0; i < 3; ++i) inst->update(0.1f);
    EXPECT_NEAR(inst->self()["distance"].get<f64>(), 1.5, 1e-9); // 3 * 0.1 * speed 5
    inst->destroy();
    EXPECT_EQ(diedTicks, 3);
    EXPECT_EQ(vm.events().subscriberCount("player.damage"), 0u); // released with the instance

    // Coroutine from onStart completed against script time.
    EXPECT_FALSE(inst->self()["warmedUp"].valid() && inst->self()["warmedUp"].get<bool>());
}

TEST(ScriptEvents, SampleScriptCoroutineWarmsUp) {
    ScriptVMConfig cfg;
    cfg.searchRoots = {OX_TEST_DATA_DIR};
    ScriptVM vm(cfg);
    LogCapture logs;
    auto inst = vm.createInstance(vm.loadScript(fs::path(OX_TEST_DATA_DIR) / "sample_player.lua"));
    ASSERT_TRUE(inst->create());
    for (int i = 0; i < 10; ++i) {
        inst->update(0.125f);
        vm.update(0.125);
    }
    EXPECT_TRUE(inst->self()["warmedUp"].get<bool>());
    EXPECT_TRUE(logs.contains("sample_player.lua:27: player warmed up at 1.0"));
    EXPECT_EQ(vm.errorCount(), 0u) << vm.lastError();
}
