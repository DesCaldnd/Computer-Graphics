#pragma once

// Runtime asset loading service (register in ox::Services).
//
//   AssetManager assets(registry /* or PakAssetSource */, &jobs);
//   AssetHandle<MaterialAsset> mat = assets.load<MaterialAsset>(uuid, kPriorityHigh);
//   mat.onLoaded([](bool ok) { ... });      // after its textures are loaded too
//   ... every frame on the main thread: assets.update();   // callbacks, hot reload swaps, LRU unloading
//
// Loads run on the JobSystem (synchronously when constructed without one). Requests are served in priority
// order. Dependencies reported by the loader/record (material -> textures) are loaded first and kept
// referenced while the asset is loaded. Unreferenced assets stay cached (LRU, `keepUnreferenced`) until
// evicted or the memory budget is exceeded. Hot reload: when the source reports a change, loaded assets are
// reloaded in the background and swapped in update(), then onReloaded fires (GPU caches re-upload).

#include <oxwald/assets/asset_data.hpp>
#include <oxwald/assets/asset_handle.hpp>
#include <oxwald/assets/asset_source.hpp>
#include <oxwald/core/events.hpp>

#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ox {
class JobSystem;
class JobHandle;
}

namespace ox::assets {

inline constexpr i32 kPriorityLow = -100;
inline constexpr i32 kPriorityNormal = 0;
inline constexpr i32 kPriorityHigh = 100;
inline constexpr i32 kPriorityCritical = 1000;

struct LoadedAsset {
    std::shared_ptr<const void> data;
    usize memory = 0;
    std::vector<Uuid> dependencies; // in addition to AssetRecord::dependencies
};

struct AssetStats {
    struct PerType {
        u32 count = 0;
        usize bytes = 0;
    };
    u32 loaded = 0;
    u32 loading = 0;
    u32 failed = 0;
    u32 unreferenced = 0; // loaded, cached, evictable
    usize memoryBytes = 0;
    usize memoryBudget = 0;
    std::map<AssetType, PerType> perType;
};

class AssetManager {
public:
    using Loader = std::function<Result<LoadedAsset>(std::vector<std::byte> bytes, const AssetRecord& record)>;

    struct Options {
        usize memoryBudget = 0;     // bytes; 0 = unlimited. Exceeding it evicts unreferenced assets (oldest first)
        u32 keepUnreferenced = 64;  // LRU size of cached unreferenced assets
        bool callbacksOnMainThread = true; // false: onLoaded callbacks run on the finishing thread
    };

    AssetManager(IAssetSource& source, JobSystem* jobs = nullptr);
    AssetManager(IAssetSource& source, JobSystem* jobs, Options options);
    ~AssetManager(); // waits for in-flight loads
    AssetManager(const AssetManager&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;

    template <class T>
    AssetHandle<T> load(const Uuid& uuid, i32 priority = kPriorityNormal) {
        return AssetHandle<T>(loadUntyped(uuid, T::kAssetType, priority, false));
    }
    template <class T>
    AssetHandle<T> load(std::string_view assetPath, i32 priority = kPriorityNormal) {
        return load<T>(m_source.uuidForPath(assetPath).value_or(Uuid{}), priority);
    }
    // Loads on the calling thread (dependencies too); returns when done.
    template <class T>
    AssetHandle<T> loadSync(const Uuid& uuid) {
        return AssetHandle<T>(loadUntyped(uuid, T::kAssetType, kPriorityCritical, true));
    }
    template <class T>
    AssetHandle<T> loadSync(std::string_view assetPath) {
        return loadSync<T>(m_source.uuidForPath(assetPath).value_or(Uuid{}));
    }
    // expectedType Unknown = whatever the record says.
    UntypedAssetHandle loadUntyped(const Uuid& uuid, AssetType expectedType = AssetType::Unknown,
                                   i32 priority = kPriorityNormal, bool synchronous = false);
    // Existing handle without starting a load (invalid when the asset is not known to the manager).
    [[nodiscard]] UntypedAssetHandle find(const Uuid& uuid);
    [[nodiscard]] AssetState state(const Uuid& uuid);

    void registerLoader(AssetType type, Loader loader);
    // Placeholder returned by AssetHandle::getOrDefault for the type.
    void setPlaceholder(AssetType type, std::shared_ptr<const void> data);
    template <class T>
    [[nodiscard]] const T* placeholder() const {
        return static_cast<const T*>(placeholderFor(T::kAssetType).get());
    }

    // Main thread, once per frame.
    void update();
    // Blocks until every queued/in-flight load (and reload) finished; then runs update().
    void waitAll();
    // Reloads a loaded asset (as on a source change).
    void reload(const Uuid& uuid);
    // Evicts every unreferenced asset now.
    usize unloadUnused();

    [[nodiscard]] AssetStats stats() const;
    [[nodiscard]] IAssetSource& source() { return m_source; }
    [[nodiscard]] const Options& options() const { return m_options; }
    void setMemoryBudget(usize bytes);

    // Main thread (update()) unless noted.
    Signal<const Uuid&, AssetType> onLoaded;
    Signal<const Uuid&, AssetType> onReloaded;
    Signal<const Uuid&, AssetType> onUnloaded;
    Signal<const Uuid&, const std::string&> onFailed;

private:
    using SlotPtr = std::shared_ptr<detail::AssetSlot>;
    struct Request {
        i32 priority;
        u64 sequence;
        SlotPtr slot;
        bool reload;
        bool operator<(const Request& o) const {
            return priority != o.priority ? priority < o.priority : sequence > o.sequence;
        }
    };
    struct Event {
        enum Kind { Loaded, Failed, Reloaded, Callback } kind;
        Uuid uuid;
        AssetType type;
        std::string error;
        std::function<void(bool)> callback;
        bool ok = false;
    };

    SlotPtr getOrCreateSlot(const Uuid& uuid, AssetType type);
    void enqueue(const SlotPtr& slot, i32 priority, bool reload);
    void pump();
    void execute(const SlotPtr& slot, bool reload, bool synchronous);
    void finish(const SlotPtr& slot, bool ok);
    void dependencyDone(const SlotPtr& waiter);
    void postEvent(Event e);
    [[nodiscard]] std::shared_ptr<const void> placeholderFor(AssetType type) const;
    void registerBuiltins();
    void evictLocked(std::vector<SlotPtr>& evicted, bool all);
    void installBuiltin(const Uuid& uuid, AssetType type, std::shared_ptr<const void> data, usize memory);

    IAssetSource& m_source;
    JobSystem* m_jobs = nullptr;
    Options m_options;
    ScopedConnection m_sourceConnection;

    mutable std::mutex m_mutex;
    std::unordered_map<Uuid, SlotPtr> m_slots;
    std::unordered_map<AssetType, Loader> m_loaders;
    std::unordered_map<AssetType, std::shared_ptr<const void>> m_placeholders;
    std::priority_queue<Request> m_queue;
    u64 m_sequence = 0;

    struct PendingReload {
        SlotPtr slot;
        LoadedAsset asset;
        std::vector<SlotPtr> dependencies;
    };
    std::vector<PendingReload> m_reloadsReady;
    std::vector<Uuid> m_reloadRequests;
    std::vector<Event> m_events;
    std::vector<std::shared_ptr<const void>> m_graveyard; // old data kept until the next update()

    std::mutex m_inflightMutex;
    std::condition_variable m_inflightCv;
    u32 m_inflight = 0;
    std::thread::id m_mainThread;
};

} // namespace ox::assets
