// Глава 08: Lua `await` на C++ Future и C++ co_await на Lua-функцию через script::AsyncBridge
// (docs/guide/08-coroutines.md). Скрипт: door.lua рядом с этим файлом.
#include <oxwald/async/async.hpp>
#include <oxwald/script/async_bridge.hpp>
#include <oxwald/script/script_instance.hpp>
#include <oxwald/script/script_vm.hpp>

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <thread>

using namespace ox;
using namespace ox::script;

namespace {

// Асинхронная «база ассетов»: каждый запрос — Promise, который завершается позже (в игре — IO-поток).
struct AssetDb {
    std::map<std::string, Promise<std::string>> requests;
    Future<std::string> loadAsync(const std::string& path) { return requests[path].future(); }
};

} // namespace

TEST(GuideCoroLuaAwait, LuaAwaitsCppAndCppAwaitsLua) {
    ScriptVM vm(ScriptVMConfig{.hotReloadInterval = 0.0});
    AsyncBridge bridge(vm); // регистрирует `await`, `async.await` и тип Future во всех песочницах
    AssetDb assets;
    // API лучше привязывать до создания экземпляров и окружений.
    vm.bindApi("assets", [&](sol::state_view, sol::table& api) {
        api["load"] = [&](const std::string& path) { return bridge.wrap(assets.loadAsync(path)); };
    });

    auto door = vm.createInstance(vm.loadScript(std::string(OX_GUIDE_DIR) + "/door.lua"));
    ASSERT_TRUE(door->create());

    CoroutineScheduler sched;
    f64 openedTo = 0.0;
    auto h = sched.spawn([&]() -> Task<> {
        // Запускает open(self, 45) как Lua-корутину, принадлежащую экземпляру; ждём её return.
        sol::main_object r = co_await bridge.invoke(*door, "open", 45.0);
        openedTo = r.as<f64>();
    });
    EXPECT_EQ(door->self()["state"].get<std::string>(), "opening");

    auto frame = [&](f64 dt) { // порядок кадра, как в движке
        bridge.update();  // будит Lua-корутины, чьи Future завершились
        vm.update(dt);    // таймеры/корутины скриптов
        door->update(static_cast<f32>(dt));
        sched.tick(dt);   // C++ корутины
    };
    frame(0.1);
    frame(0.1);
    EXPECT_EQ(door->self()["state"].get<std::string>(), "opening"); // ждёт ассет, не опрашивая

    std::thread([&] { assets.requests["anims/door_open.anim"].setValue("DoorOpenClip"); }).join();
    assets.requests["sfx/missing.ogg"].setError("file not found");
    for (int i = 0; i < 40 && h.isRunning(); ++i) frame(0.1); // 90° / 45°/с = 2 с скрипта
    EXPECT_EQ(h.status(), CoroutineStatus::Completed);
    EXPECT_DOUBLE_EQ(openedTo, 90.0);
    EXPECT_EQ(door->self()["clip"].get<std::string>(), "DoorOpenClip");
    EXPECT_NE(door->self()["loadError"].get<std::string>().find("file not found"), std::string::npos);
    door->destroy();
}
