#include <oxwald/core/file_watcher.hpp>

#include <oxwald/core/log.hpp>

#include <algorithm>
#include <cctype>
#include <set>

namespace fs = std::filesystem;

namespace ox {

namespace {

std::string lowerExtension(std::string ext) {
    if (!ext.empty() && ext.front() != '.') {
        ext.insert(ext.begin(), '.');
    }
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

} // namespace

FileWatcher::FileWatcher() = default;

FileWatcher::~FileWatcher() { stop(); }

FileWatcher::WatchId FileWatcher::watchFile(const fs::path& path, Callback callback) {
    auto w = std::make_shared<Watch>();
    w->path = path.lexically_normal();
    w->directory = false;
    w->callback = std::move(callback);
    w->committed = scan(*w);
    std::lock_guard lock(m_mutex);
    const WatchId id = m_nextId++;
    m_watches.emplace(id, std::move(w));
    return id;
}

FileWatcher::WatchId FileWatcher::watchDirectory(const fs::path& dir, Callback callback, bool recursive,
                                                 std::vector<std::string> extensions) {
    auto w = std::make_shared<Watch>();
    w->path = dir.lexically_normal();
    w->directory = true;
    w->recursive = recursive;
    for (std::string& e : extensions) {
        w->extensions.push_back(lowerExtension(std::move(e)));
    }
    w->callback = std::move(callback);
    std::error_code ec;
    if (!fs::is_directory(w->path, ec)) {
        OX_LOG_WARN("filewatcher", "watching '{}' which is not (yet) a directory", w->path.string());
    }
    w->committed = scan(*w);
    std::lock_guard lock(m_mutex);
    const WatchId id = m_nextId++;
    m_watches.emplace(id, std::move(w));
    return id;
}

void FileWatcher::unwatch(WatchId id) {
    std::lock_guard lock(m_mutex);
    m_watches.erase(id);
    std::erase_if(m_queue, [id](const Queued& q) { return q.id == id; });
}

usize FileWatcher::watchCount() const {
    std::lock_guard lock(m_mutex);
    return m_watches.size();
}

void FileWatcher::setDebounce(std::chrono::milliseconds debounce) {
    std::lock_guard lock(m_mutex);
    m_debounce = std::max(debounce, std::chrono::milliseconds(0));
}

std::chrono::milliseconds FileWatcher::debounce() const {
    std::lock_guard lock(m_mutex);
    return m_debounce;
}

std::map<fs::path, FileWatcher::FileState> FileWatcher::scan(const Watch& w) {
    std::map<fs::path, FileState> out;
    const auto stat = [](const fs::path& p, const fs::directory_entry* entry) -> FileState {
        std::error_code ec;
        FileState s;
        const bool regular = entry ? entry->is_regular_file(ec) : fs::is_regular_file(p, ec);
        if (ec || !regular) {
            return s;
        }
        const auto size = fs::file_size(p, ec);
        if (ec) {
            return s;
        }
        const auto time = fs::last_write_time(p, ec);
        if (ec) {
            return s;
        }
        s.exists = true;
        s.size = static_cast<u64>(size);
        s.mtime = static_cast<i64>(time.time_since_epoch().count());
        return s;
    };

    if (!w.directory) {
        out.emplace(w.path, stat(w.path, nullptr));
        return out;
    }
    const auto consider = [&](const fs::directory_entry& e) {
        if (!w.extensions.empty()) {
            const std::string ext = lowerExtension(e.path().extension().string());
            if (std::find(w.extensions.begin(), w.extensions.end(), ext) == w.extensions.end()) {
                return;
            }
        }
        FileState s = stat(e.path(), &e);
        if (s.exists) {
            out.emplace(e.path().lexically_normal(), s);
        }
    };
    std::error_code ec;
    if (!fs::is_directory(w.path, ec)) {
        return out;
    }
    if (w.recursive) {
        for (fs::recursive_directory_iterator it(w.path, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            consider(*it);
        }
    } else {
        for (fs::directory_iterator it(w.path, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
             it.increment(ec)) {
            consider(*it);
        }
    }
    return out;
}

void FileWatcher::scanAll(std::vector<Queued>& out) {
    std::lock_guard scanLock(m_scanMutex);
    std::vector<std::pair<WatchId, std::shared_ptr<Watch>>> watches;
    std::chrono::milliseconds debounce;
    {
        std::lock_guard lock(m_mutex);
        watches.assign(m_watches.begin(), m_watches.end());
        debounce = m_debounce;
    }
    const auto now = std::chrono::steady_clock::now();
    for (auto& [id, w] : watches) {
        const std::map<fs::path, FileState> current = scan(*w);

        std::set<fs::path> paths;
        for (const auto& [p, s] : current) {
            paths.insert(p);
        }
        for (const auto& [p, s] : w->committed) {
            paths.insert(p);
        }
        for (const auto& [p, s] : w->pending) {
            paths.insert(p);
        }

        for (const fs::path& p : paths) {
            const auto cIt = current.find(p);
            const FileState cur = cIt != current.end() ? cIt->second : FileState{};
            const auto kIt = w->committed.find(p);
            const FileState prev = kIt != w->committed.end() ? kIt->second : FileState{};
            if (cur == prev) {
                w->pending.erase(p);
                continue;
            }
            auto pIt = w->pending.find(p);
            if (pIt == w->pending.end() || !(pIt->second.state == cur)) {
                // New or still changing: (re)start the debounce window.
                pIt = w->pending.insert_or_assign(p, Pending{cur, now}).first;
            }
            if (now - pIt->second.since < debounce) {
                continue;
            }
            FileChange change;
            change.path = p;
            change.kind = !prev.exists ? FileChange::Kind::Added
                                       : (!cur.exists ? FileChange::Kind::Removed : FileChange::Kind::Modified);
            out.push_back(Queued{id, std::move(change)});
            w->pending.erase(pIt);
            if (cur.exists) {
                w->committed[p] = cur;
            } else if (w->directory) {
                w->committed.erase(p);
            } else {
                w->committed[p] = cur; // single-file watches keep their (missing) entry
            }
        }
    }
}

void FileWatcher::deliver(std::vector<Queued>& changes, usize& fired) {
    for (Queued& q : changes) {
        Callback cb;
        {
            std::lock_guard lock(m_mutex);
            auto it = m_watches.find(q.id);
            if (it == m_watches.end()) {
                continue; // unwatched in the meantime (possibly by an earlier callback)
            }
            cb = it->second->callback;
        }
        if (cb) {
            cb(q.change);
            ++fired;
        }
    }
}

usize FileWatcher::poll() {
    std::vector<Queued> changes;
    bool background;
    {
        std::lock_guard lock(m_mutex);
        background = m_running;
        changes.swap(m_queue);
    }
    if (!background) {
        scanAll(changes);
    }
    usize fired = 0;
    deliver(changes, fired);
    return fired;
}

void FileWatcher::start(std::chrono::milliseconds interval) {
    std::lock_guard lock(m_mutex);
    if (m_running) {
        return;
    }
    m_running = true;
    m_stopRequested = false;
    m_thread = std::thread([this, interval] { backgroundLoop(interval); });
}

void FileWatcher::stop() {
    {
        std::lock_guard lock(m_mutex);
        if (!m_running) {
            return;
        }
        m_stopRequested = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    std::lock_guard lock(m_mutex);
    m_running = false;
}

bool FileWatcher::running() const {
    std::lock_guard lock(m_mutex);
    return m_running;
}

void FileWatcher::backgroundLoop(std::chrono::milliseconds interval) {
    for (;;) {
        std::vector<Queued> changes;
        scanAll(changes);
        std::unique_lock lock(m_mutex);
        for (Queued& q : changes) {
            if (m_watches.contains(q.id)) {
                m_queue.push_back(std::move(q));
            }
        }
        if (m_cv.wait_for(lock, interval, [this] { return m_stopRequested; })) {
            return;
        }
    }
}

} // namespace ox
