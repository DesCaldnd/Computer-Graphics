// Глава 15: ScriptAsset + ScriptInstance — свойства для редактора, жизненный цикл, события, корутины
// и таймеры скрипта (docs/guide/15-scripting-lua.md). Скрипт: scripts/turret.lua.
#include <oxwald/script/script_events.hpp>
#include <oxwald/script/script_instance.hpp>
#include <oxwald/script/script_vm.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

using namespace ox;
using namespace ox::script;

namespace {
const std::filesystem::path kScripts = std::filesystem::path(OX_GUIDE_DIR) / "scripts";
} // namespace

TEST(GuideLuaInstance, PropertiesForTheInspector) {
    ScriptVM vm({.searchRoots = {kScripts}});
    std::shared_ptr<ScriptAsset> turret = vm.loadScript(kScripts / "turret.lua"); // кэшируется по пути
    ASSERT_TRUE(turret->valid());

    // Объявленные свойства доступны без экземпляра: это то, что рисует инспектор.
    const auto& props = turret->properties(); // отсортированы по (order, name)
    ASSERT_EQ(props.size(), 6u);
    EXPECT_EQ(props[0].name, "friendly"); // короткая форма: order = 0
    EXPECT_EQ(props[1].name, "range");
    const ScriptPropertyDesc* range = turret->findProperty("range");
    ASSERT_NE(range, nullptr);
    EXPECT_EQ(range->type, ScriptPropertyType::Float);
    EXPECT_EQ(range->max, 50.0);
    EXPECT_EQ(range->tooltip, "Дальность, м");
    EXPECT_EQ(turret->findProperty("tint")->type, ScriptPropertyType::Color);
}

TEST(GuideLuaInstance, TurretLifecycle) {
    ScriptVM vm({.hotReloadInterval = 0.0, .searchRoots = {kScripts}});
    auto turret = vm.createInstance(vm.loadScript(kScripts / "turret.lua"), [](ScriptInstance&, sol::table& self) {
        self["entityName"] = "Turret_01"; // init: до onCreate (в ECS сюда кладут ссылку на сущность)
    });

    // Переопределения «из префаба»: тип приводится, значение зажимается в [min, max].
    EXPECT_TRUE(turret->setProperty("ammo", ScriptValue{i64{3}}));
    EXPECT_TRUE(turret->setProperty("range", ScriptValue{999.0}));
    EXPECT_FALSE(turret->setProperty("unknown", ScriptValue{1.0}));
    ASSERT_TRUE(turret->create()); // выполняет chunk, заполняет self, вызывает onCreate(self)
    EXPECT_EQ(std::get<f64>(turret->getProperty("range")), 50.0);

    int fired = 0, destroyedShots = -1;
    vm.events().subscribe("turret.fired", [&](std::string_view, const sol::object&) { ++fired; });
    vm.events().subscribe("turret.destroyed", [&](std::string_view, const sol::object& p) {
        destroyedShots = p.as<sol::table>()["shots"].get<int>();
    });

    // Враг в зоне видимости: событие из C++ с таблицей в payload.
    sol::table enemy = vm.lua().create_table();
    enemy["distance"] = 12.0;
    enemy["position"] = glm::vec3(10.f, 1.5f, 0.f);
    vm.events().publish("enemy.spotted", enemy);

    auto frame = [&](f32 dt) {
        vm.update(dt);        // время скрипта: wait(), таймеры, корутины, hot reload
        turret->update(dt);   // onStart (один раз), затем onUpdate(self, dt)
    };
    frame(0.25f);
    EXPECT_EQ(turret->self()["state"].get<std::string>(), "warming");
    for (int i = 0; i < 20; ++i) frame(0.25f); // 5 с
    EXPECT_EQ(fired, 3); // кончились патроны
    EXPECT_NEAR(turret->self()["yaw"].get<f64>(), 90.0, 1e-3); // враг по +X

    turret->sendEvent("reload", ScriptValue{i64{2}}); // -> onEvent(self, "reload", 2)
    for (int i = 0; i < 8; ++i) frame(0.25f);
    EXPECT_EQ(fired, 5);

    turret->destroy(); // onDestroy + отмена корутин/таймеров/подписок экземпляра
    EXPECT_EQ(destroyedShots, 5);
    EXPECT_EQ(vm.events().subscriberCount("enemy.spotted"), 0u);
    EXPECT_EQ(vm.errorCount(), 0u) << vm.lastError();
}

TEST(GuideLuaInstance, InvokeAnyScriptFunction) {
    ScriptVM vm;
    auto asset = vm.loadScriptFromString("door.lua", R"(
        properties = { angle = 90 }
        function onCreate(self) self.open = false end
        function toggle(self, by) self.open = not self.open; return by .. " toggled the door" end
    )");
    auto door = vm.createInstance(asset);
    ASSERT_TRUE(door->create());
    ScriptResult r = door->invoke("toggle", "Player"); // toggle(self, "Player")
    ASSERT_TRUE(r) << r.error;
    EXPECT_EQ(r.value.as<std::string>(), "Player toggled the door");
    EXPECT_TRUE(door->self()["open"].get<bool>());
}

TEST(GuideLuaInstance, TimersAndErrorsDoNotStopTheGame) {
    ScriptVM vm;
    auto asset = vm.loadScriptFromString("beacon.lua", R"(
        function onCreate(self)
            self.blinks = 0
            self.blinkTimer = timer.every(0.5, function() self.blinks = self.blinks + 1 end)
            timer.after(1.2, function() timer.cancel(self.blinkTimer) end)
        end
        function onUpdate(self, dt) if self.blinks == 1 then error("oops") end end
    )");
    auto beacon = vm.createInstance(asset);
    ASSERT_TRUE(beacon->create());
    for (int i = 0; i < 16; ++i) {
        vm.update(0.125);
        beacon->update(0.125f);
    }
    EXPECT_EQ(beacon->self()["blinks"].get<int>(), 2); // t=0.5 и t=1.0, потом таймер отменён
    EXPECT_GT(beacon->errorCount(), 0u);                // ошибки в onUpdate залогированы и посчитаны...
    EXPECT_EQ(beacon->state(), ScriptInstance::State::Started); // ...но экземпляр продолжает работать
}
