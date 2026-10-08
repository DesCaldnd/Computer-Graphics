// Глава 15: горячая перезагрузка скриптов — состояние self сохраняется, новые свойства получают значения
// по умолчанию, вызывается on_reload; синтаксическая ошибка оставляет старую версию
// (docs/guide/15-scripting-lua.md).
#include <oxwald/script/script_instance.hpp>
#include <oxwald/script/script_vm.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace ox;
using namespace ox::script;
namespace fs = std::filesystem;

namespace {

// «Сохранение файла в редакторе». Сдвигаем mtime, чтобы изменение было видно на ФС с грубыми отметками времени.
void saveScript(const fs::path& path, const std::string& text, int bumpSeconds) {
    {
        std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
    }
    if (bumpSeconds != 0) fs::last_write_time(path, fs::last_write_time(path) + std::chrono::seconds(bumpSeconds));
}

} // namespace

TEST(GuideLuaHotReload, EditWhilePlaying) {
    const fs::path dir = fs::temp_directory_path() /
                         ("oxwald_guide_hot_reload_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    const fs::path file = dir / "coin_counter.lua";
    saveScript(file, R"(
        properties = { perCoin = { type = "int", default = 1 } }
        function onCreate(self) self.coins = 0 end
        function onEvent(self, name) if name == "coin" then self.coins = self.coins + self.perCoin end end
    )", 0);

    ScriptVM vm({.hotReloadInterval = 0.0}); // 0 = опрос вручную; в игре — каждые 0.5 с внутри vm.update()
    auto asset = vm.loadScript(file);
    auto counter = vm.createInstance(asset);
    ASSERT_TRUE(counter->create());
    counter->sendEvent("coin");
    counter->sendEvent("coin");
    EXPECT_EQ(counter->self()["coins"].get<i64>(), 2);

    // Дизайнер правит скрипт: монеты дороже + новое свойство + хук on_reload.
    saveScript(file, R"(
        properties = { perCoin = { type = "int", default = 1 }, bonus = { type = "int", default = 10 } }
        function onCreate(self) self.coins = 0 end
        function onEvent(self, name) if name == "coin" then self.coins = self.coins + self.perCoin + self.bonus end end
        function on_reload(self) self.reloaded = true end
    )", 2);
    EXPECT_EQ(vm.pollHotReload(), 1u);                         // число перезагруженных ассетов
    EXPECT_EQ(counter->self()["coins"].get<i64>(), 2);         // состояние сохранено
    EXPECT_TRUE(counter->self()["reloaded"].get<bool>());      // on_reload(self) вызван
    EXPECT_EQ(asset->findProperty("bonus")->defaultValue, ScriptValue{i64{10}}); // инспектор видит новое свойство
    counter->sendEvent("coin");
    EXPECT_EQ(counter->self()["coins"].get<i64>(), 13);        // работает новый код

    // Ошибка в правке: старая версия продолжает работать, ошибка в логе.
    saveScript(file, "function onEvent(self, name) self.coins = = 0 end", 4);
    EXPECT_EQ(vm.pollHotReload(), 0u);
    counter->sendEvent("coin");
    EXPECT_EQ(counter->self()["coins"].get<i64>(), 24);
    EXPECT_EQ(counter->loadedVersion(), 2u);

    counter.reset();
    asset.reset();
    fs::remove_all(dir);
}
