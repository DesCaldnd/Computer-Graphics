// Глава 01: отслеживание изменений файлов (hot reload) (docs/guide/01-core.md).
#include <oxwald/core/file_watcher.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

TEST(GuideCoreFiles, WatcherReportsNewAndChangedFiles) {
    namespace fs = std::filesystem;
    using namespace std::chrono_literals;
    const fs::path dir = fs::temp_directory_path() / "oxwald_guide_watcher";
    fs::remove_all(dir);
    fs::create_directories(dir);

    std::vector<ox::FileChange> changes;
    ox::FileWatcher watcher;
    watcher.setDebounce(20ms); // по умолчанию 100 мс: редакторы пишут файлы в несколько приёмов
    watcher.watchDirectory(dir, [&](const ox::FileChange& c) { changes.push_back(c); },
                           /*recursive*/ true, {".lua"}); // только .lua

    std::ofstream(dir / "enemy.lua") << "return {}";
    std::ofstream(dir / "readme.txt") << "ignored";

    // poll() вызывается раз в кадр; колбэки выполняются в потоке, который вызвал poll().
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (changes.empty() && std::chrono::steady_clock::now() < deadline) {
        watcher.poll();
        std::this_thread::sleep_for(10ms);
    }
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].kind, ox::FileChange::Kind::Added);
    EXPECT_EQ(changes[0].path.filename(), "enemy.lua");

    fs::remove_all(dir);
}
