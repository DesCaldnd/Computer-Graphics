#pragma once

// Portable polling file watcher (mtime + size). Typical use: shader/asset hot reload.
//
//   FileWatcher watcher;
//   watcher.watchDirectory(shaderDir, [](const FileChange& c) { reload(c.path); }, true, {".glsl"});
//   ... each frame: watcher.poll();
//
// A change is reported once the file's (exists, mtime, size) state differs from the last reported
// state and has stayed unchanged for the debounce interval (editors often write files in several
// steps). Callbacks always run on the thread calling poll(). In background mode (start()), a thread
// scans every `interval` and poll() only delivers the queued changes. Existing files are not reported
// when a watch is added. All methods are thread-safe; callbacks may call watch/unwatch.

#include <oxwald/core/types.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ox {

struct FileChange {
    enum class Kind { Added, Modified, Removed };
    std::filesystem::path path;
    Kind kind = Kind::Modified;
};

class FileWatcher {
public:
    using FileChange = ox::FileChange;
    using Callback = std::function<void(const FileChange&)>;
    using WatchId = u64;
    static constexpr WatchId kInvalidWatch = 0;

    FileWatcher();
    ~FileWatcher();
    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    // The file need not exist yet (its creation is reported as Added).
    WatchId watchFile(const std::filesystem::path& path, Callback callback);
    // extensions: e.g. {".glsl", "png"} (leading dot optional, case-insensitive); empty = all files.
    WatchId watchDirectory(const std::filesystem::path& dir, Callback callback, bool recursive = true,
                           std::vector<std::string> extensions = {});
    void unwatch(WatchId id);
    [[nodiscard]] usize watchCount() const;

    void setDebounce(std::chrono::milliseconds debounce);
    [[nodiscard]] std::chrono::milliseconds debounce() const;

    // Scans (unless the background thread is running) and fires callbacks. Returns callbacks fired.
    usize poll();

    void start(std::chrono::milliseconds interval = std::chrono::milliseconds(250));
    void stop();
    [[nodiscard]] bool running() const;

private:
    struct FileState {
        bool exists = false;
        i64 mtime = 0;
        u64 size = 0;
        bool operator==(const FileState&) const = default;
    };
    struct Pending {
        FileState state;
        std::chrono::steady_clock::time_point since;
    };
    struct Watch {
        std::filesystem::path path;
        bool directory = false;
        bool recursive = false;
        std::vector<std::string> extensions;
        Callback callback;
        std::map<std::filesystem::path, FileState> committed;
        std::map<std::filesystem::path, Pending> pending;
    };
    struct Queued {
        WatchId id;
        FileChange change;
    };

    static std::map<std::filesystem::path, FileState> scan(const Watch& w);
    // Scans every watch and appends ready changes. Serialised by m_scanMutex; disk I/O happens
    // without holding m_mutex.
    void scanAll(std::vector<Queued>& out);
    void deliver(std::vector<Queued>& changes, usize& fired);
    void backgroundLoop(std::chrono::milliseconds interval);

    std::mutex m_scanMutex;
    mutable std::mutex m_mutex;
    std::map<WatchId, std::shared_ptr<Watch>> m_watches;
    std::vector<Queued> m_queue; // filled by the background thread
    WatchId m_nextId = 1;
    std::chrono::milliseconds m_debounce{100};

    std::thread m_thread;
    std::condition_variable m_cv;
    bool m_stopRequested = false;
    bool m_running = false;
};

} // namespace ox
