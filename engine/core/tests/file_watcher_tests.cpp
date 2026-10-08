#include <oxwald/core/file_watcher.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <format>
#include <fstream>
#include <random>
#include <thread>

namespace fs = std::filesystem;
using namespace ox;
using namespace std::chrono_literals;

namespace {

class TempDir {
public:
    TempDir() {
        std::random_device rd;
        m_path = fs::temp_directory_path() /
                 std::format("oxwald_fw_{}_{}", std::chrono::steady_clock::now().time_since_epoch().count(), rd());
        fs::create_directories(m_path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(m_path, ec);
    }
    const fs::path& path() const { return m_path; }

private:
    fs::path m_path;
};

void writeFile(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary | std::ios::trunc) << content;
}

// Moves mtime forward explicitly so changes are detectable regardless of file system timestamp resolution.
void bumpMtime(const fs::path& p, int seconds = 10) {
    fs::last_write_time(p, fs::last_write_time(p) + std::chrono::seconds(seconds));
}

struct Recorder {
    std::vector<FileChange> changes;
    FileWatcher::Callback callback() {
        return [this](const FileChange& c) { changes.push_back(c); };
    }
    usize count(FileChange::Kind kind) const {
        return static_cast<usize>(std::count_if(changes.begin(), changes.end(), [&](const auto& c) { return c.kind == kind; }));
    }
};

} // namespace

TEST(FileWatcher, WatchFileDetectsModifyRemoveAdd) {
    TempDir dir;
    const fs::path file = dir.path() / "shader.glsl";
    writeFile(file, "v1");

    FileWatcher watcher;
    watcher.setDebounce(0ms);
    Recorder rec;
    const auto id = watcher.watchFile(file, rec.callback());
    EXPECT_NE(id, FileWatcher::kInvalidWatch);
    EXPECT_EQ(watcher.poll(), 0u); // existing file is not reported

    writeFile(file, "version 2"); // size changes
    EXPECT_EQ(watcher.poll(), 1u);
    ASSERT_EQ(rec.changes.size(), 1u);
    EXPECT_EQ(rec.changes[0].kind, FileChange::Kind::Modified);
    EXPECT_EQ(rec.changes[0].path, file.lexically_normal());
    EXPECT_EQ(watcher.poll(), 0u); // reported once

    bumpMtime(file); // same size, newer mtime
    EXPECT_EQ(watcher.poll(), 1u);
    EXPECT_EQ(rec.changes.back().kind, FileChange::Kind::Modified);

    fs::remove(file);
    EXPECT_EQ(watcher.poll(), 1u);
    EXPECT_EQ(rec.changes.back().kind, FileChange::Kind::Removed);

    writeFile(file, "v3");
    EXPECT_EQ(watcher.poll(), 1u);
    EXPECT_EQ(rec.changes.back().kind, FileChange::Kind::Added);

    watcher.unwatch(id);
    EXPECT_EQ(watcher.watchCount(), 0u);
    writeFile(file, "v4 longer");
    EXPECT_EQ(watcher.poll(), 0u);
}

TEST(FileWatcher, WatchDirectoryRecursiveWithExtensions) {
    TempDir dir;
    writeFile(dir.path() / "a.glsl", "a");
    FileWatcher watcher;
    watcher.setDebounce(0ms);
    Recorder all, glslOnly, flat;
    watcher.watchDirectory(dir.path(), all.callback(), true);
    watcher.watchDirectory(dir.path(), glslOnly.callback(), true, {"GLSL"});
    watcher.watchDirectory(dir.path(), flat.callback(), false, {".glsl"});

    writeFile(dir.path() / "sub" / "b.glsl", "b");
    writeFile(dir.path() / "notes.txt", "n");
    watcher.poll();
    EXPECT_EQ(all.count(FileChange::Kind::Added), 2u);
    EXPECT_EQ(glslOnly.count(FileChange::Kind::Added), 1u);
    EXPECT_EQ(glslOnly.changes.at(0).path.filename(), "b.glsl");
    EXPECT_TRUE(flat.changes.empty()); // non-recursive watch ignores sub/

    writeFile(dir.path() / "a.glsl", "a modified");
    fs::remove(dir.path() / "sub" / "b.glsl");
    watcher.poll();
    EXPECT_EQ(all.count(FileChange::Kind::Modified), 1u);
    EXPECT_EQ(all.count(FileChange::Kind::Removed), 1u);
    EXPECT_EQ(glslOnly.count(FileChange::Kind::Removed), 1u);
    ASSERT_EQ(flat.changes.size(), 1u);
    EXPECT_EQ(flat.changes[0].kind, FileChange::Kind::Modified);
}

TEST(FileWatcher, DebounceWaitsForStableState) {
    TempDir dir;
    const fs::path file = dir.path() / "data.json";
    writeFile(file, "1");
    FileWatcher watcher;
    watcher.setDebounce(100ms);
    EXPECT_EQ(watcher.debounce(), 100ms);
    Recorder rec;
    watcher.watchFile(file, rec.callback());

    writeFile(file, "12");
    EXPECT_EQ(watcher.poll(), 0u); // change seen, debounce window starts
    std::this_thread::sleep_for(30ms);
    writeFile(file, "123"); // still being written: window restarts
    EXPECT_EQ(watcher.poll(), 0u);
    std::this_thread::sleep_for(30ms);
    EXPECT_EQ(watcher.poll(), 0u); // only ~30 ms stable
    std::this_thread::sleep_for(120ms);
    EXPECT_EQ(watcher.poll(), 1u);
    EXPECT_EQ(rec.changes.size(), 1u);
}

TEST(FileWatcher, ChangeRevertedWithinDebounceIsNotReported) {
    TempDir dir;
    const fs::path file = dir.path() / "x.txt";
    writeFile(file, "same");
    const auto originalTime = fs::last_write_time(file);
    FileWatcher watcher;
    watcher.setDebounce(20ms);
    Recorder rec;
    watcher.watchFile(file, rec.callback());
    writeFile(file, "different");
    watcher.poll();
    writeFile(file, "same");
    fs::last_write_time(file, originalTime);
    std::this_thread::sleep_for(30ms);
    watcher.poll();
    EXPECT_TRUE(rec.changes.empty());
}

TEST(FileWatcher, CallbackMayUnwatch) {
    TempDir dir;
    FileWatcher watcher;
    watcher.setDebounce(0ms);
    int calls = 0;
    FileWatcher::WatchId id = 0;
    id = watcher.watchDirectory(dir.path(), [&](const FileChange&) {
        ++calls;
        watcher.unwatch(id);
    });
    writeFile(dir.path() / "1.txt", "1");
    writeFile(dir.path() / "2.txt", "2");
    watcher.poll();
    EXPECT_EQ(calls, 1); // second change dropped because the watch is gone
}

TEST(FileWatcher, BackgroundThreadDeliversOnPoll) {
    TempDir dir;
    FileWatcher watcher;
    watcher.setDebounce(0ms);
    Recorder rec;
    watcher.watchDirectory(dir.path(), rec.callback());
    watcher.start(5ms);
    EXPECT_TRUE(watcher.running());
    // Create atomically (rename) so the scanner thread cannot observe a half-written file.
    TempDir staging;
    writeFile(staging.path() / "bg.txt", "x");
    fs::rename(staging.path() / "bg.txt", dir.path() / "bg.txt");

    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (rec.changes.empty() && std::chrono::steady_clock::now() < deadline) {
        watcher.poll(); // delivers queued changes on this thread
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_EQ(rec.changes.size(), 1u);
    EXPECT_EQ(rec.changes[0].kind, FileChange::Kind::Added);
    watcher.stop();
    EXPECT_FALSE(watcher.running());

    // After stop, poll() scans synchronously again.
    writeFile(dir.path() / "fg.txt", "y");
    EXPECT_EQ(watcher.poll(), 1u);
}
