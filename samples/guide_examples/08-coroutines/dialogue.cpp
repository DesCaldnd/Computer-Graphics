// Глава 08: диалог с NPC (whenAny: выбор игрока или тайм-аут), загрузка уровня с прогрессом (whenAll),
// timeout() для операции, которая может зависнуть (docs/guide/08-coroutines.md).
#include <oxwald/async/async.hpp>
#include <oxwald/core/events.hpp>

#include <gtest/gtest.h>

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace ox;

namespace {

// ------------------------------------------------------------------ диалог

struct DialogueUi {
    Signal<int> onChoice; // индекс выбранного варианта
    std::vector<std::string> lines;
    bool visible = false;
    void show(std::string line) {
        lines.push_back(std::move(line));
        visible = true;
    }
    void hide() { visible = false; }
};

struct Npc {
    std::string name;
    bool questGiven = false;
};

Task<> talk(Npc& npc, DialogueUi& ui) {
    co_await named("Dialogue:" + npc.name);
    ui.show("Торговать / Задание / Пока");
    // Кто первый: выбор игрока или 30 с реального времени бездействия. Проигравший отменяется.
    auto r = co_await whenAny(toTask(event(ui.onChoice)), toTask(realSeconds(30.0)));
    const int choice = r.index == 0 ? std::get<0>(r.value) : 2; // молчание = «Пока»
    if (choice == 1) {
        ui.show("Принесёшь десять волчьих шкур?  Да / Нет");
        if (co_await event(ui.onChoice) == 0) npc.questGiven = true;
    }
    ui.hide();
}

// ------------------------------------------------------------------ загрузка уровня

struct AssetLoader { // callback-API, завершающий загрузки «потом» (в игре — с IO-потока)
    std::vector<std::pair<std::string, std::function<void(std::string)>>> pending;
    void loadAsync(std::string path, std::function<void(std::string)> done) {
        pending.emplace_back(std::move(path), std::move(done));
    }
    void completeOne() {
        auto [path, done] = std::move(pending.front());
        pending.erase(pending.begin());
        done("mesh:" + path);
    }
};

Task<std::vector<std::string>> loadLevel(AssetLoader& loader, std::vector<std::string> paths, f32& progress) {
    const usize total = paths.size();
    usize done = 0;
    auto loadOne = [&](std::string path) -> Task<std::string> {
        std::string data = co_await awaitCallback<std::string>([&](auto resume) { loader.loadAsync(path, resume); });
        ++done;
        co_return data;
    };
    auto showProgress = [&]() -> Task<> {
        while (done < total) {
            progress = static_cast<f32>(done) / static_cast<f32>(total);
            co_await nextFrame();
        }
        progress = 1.f;
    };
    std::vector<Task<std::string>> loads;
    for (const std::string& p : paths) loads.push_back(loadOne(p));
    // Все загрузки параллельно + полоска прогресса; результат — кортеж (vector<string>, monostate).
    auto results = co_await whenAll(whenAll(std::move(loads)), showProgress());
    co_return std::move(std::get<0>(results));
}

} // namespace

TEST(GuideCoroDialogue, PlayerAcceptsQuest) {
    CoroutineScheduler sched;
    DialogueUi ui;
    Npc smith{"Smith"};
    auto h = sched.spawn(talk(smith, ui));
    ui.onChoice.emit(1); // «Задание»
    sched.tick(0.016);
    EXPECT_EQ(ui.lines.size(), 2u);
    ui.onChoice.emit(0); // «Да»
    sched.tick(0.016);
    EXPECT_TRUE(smith.questGiven);
    EXPECT_FALSE(ui.visible);
    EXPECT_TRUE(h.isDone());
}

TEST(GuideCoroDialogue, IdlePlayerTimesOut) {
    CoroutineScheduler sched;
    DialogueUi ui;
    Npc smith{"Smith"};
    auto h = sched.spawn(talk(smith, ui));
    for (int i = 0; i < 31; ++i) sched.tick(1.0);
    EXPECT_TRUE(h.isDone());
    EXPECT_FALSE(ui.visible);
    EXPECT_EQ(ui.onChoice.slotCount(), 0u); // ветка event() отменена и отписалась
}

TEST(GuideCoroDialogue, LevelLoadingWithProgress) {
    CoroutineScheduler sched;
    AssetLoader loader;
    f32 progress = 0.f;
    std::vector<std::string> meshes;
    auto h = sched.spawn([&]() -> Task<> {
        meshes = co_await loadLevel(loader, {"rock.glb", "tree.glb", "hut.glb", "well.glb"}, progress);
    });
    ASSERT_EQ(loader.pending.size(), 4u); // все запросы ушли сразу
    loader.completeOne();
    loader.completeOne();
    sched.tick(0.016);
    sched.tick(0.016);
    EXPECT_FLOAT_EQ(progress, 0.5f);
    loader.completeOne();
    loader.completeOne();
    sched.tick(0.016);
    sched.tick(0.016);
    EXPECT_TRUE(h.isDone());
    EXPECT_FLOAT_EQ(progress, 1.f);
    EXPECT_EQ(meshes, (std::vector<std::string>{"mesh:rock.glb", "mesh:tree.glb", "mesh:hut.glb", "mesh:well.glb"}));
}

TEST(GuideCoroDialogue, TimeoutGivesUpOnHangingLoad) {
    CoroutineScheduler sched;
    AssetLoader loader; // загрузка никогда не завершится
    std::optional<std::string> mesh = "unset";
    auto h = sched.spawn([&]() -> Task<> {
        mesh = co_await timeout(
            awaitCallback<std::string>([&](auto resume) { loader.loadAsync("huge.glb", resume); }), 2.0);
    });
    for (int i = 0; i < 3; ++i) sched.tick(1.0);
    EXPECT_TRUE(h.isDone());
    EXPECT_FALSE(mesh.has_value()); // nullopt = истёк тайм-аут
}
