#pragma once

#include <oxwald/core/events.hpp>
#include <oxwald/core/jobs.hpp>
#include <oxwald/core/result.hpp>
#include <oxwald/core/serial/archive.hpp>
#include <oxwald/core/uuid.hpp>

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

// Save-game framework (see docs/dev/modules/runtime.md).
//
// One OXB1 file per slot under user://saves/ ("<slot>.oxsave", previous version kept as "<slot>.oxsave.bak"):
//   Document kind "savegame", version = SaveGameConfig::version (game data version), root object "SaveGame":
//     header    : SaveGameHeader (slot, display name, timestamp, play time, level, game/engine version, kind)
//     thumbnail : blob (PNG bytes from the thumbnail provider)
//     world     : { level, entities: [ {id, name, parent, full, components: {name: {...}}} ], destroyed: [uuid] }
//     sections  : map id -> object written by ISaveable::save through serial::Writer
// Entities with SaveGameComponent are stored completely ("full": re-created on load if missing); other entities
// store only components/fields marked attr::SaveGame (applied onto the level's entity with the same UUID).
// Level entities that no longer exist are listed in "destroyed".
namespace ox {

class World;
class JobSystem;
class Vfs;

enum class SaveKind : u8 { Manual, Auto, Quick };

struct SaveGameHeader {
    std::string slot;
    std::string displayName;
    i64 timestamp = 0;          // unix seconds (UTC)
    f64 playTimeSeconds = 0.0;
    std::string level;          // scene URI the world was loaded from
    std::string gameVersion;
    std::string engineVersion;
    u32 dataVersion = 0;        // SaveGameConfig::version at save time (before migrations)
    SaveKind kind = SaveKind::Manual;
    u64 entityCount = 0;
};

struct SaveSlotInfo {
    SaveGameHeader header;
    std::filesystem::path path;
    u64 fileSize = 0;
    bool hasThumbnail = false;
    bool corrupted = false;   // main file unreadable (backup may still be valid)
    bool hasBackup = false;
};

// User data section (quests, inventory, ...). save() runs on the game thread while the snapshot is taken;
// load() runs on the game thread when the save is applied.
class ISaveable {
public:
    virtual ~ISaveable() = default;
    [[nodiscard]] virtual std::string saveId() const = 0;
    virtual void save(serial::Writer& writer) const = 0;
    // Called with the section's reader (absent sections are not loaded; onMissing() is called instead).
    virtual bool load(serial::Reader& reader) = 0;
    virtual void onMissing() {}
};

struct SaveGameConfig {
    std::string directoryUri = "user://saves"; // resolved through the Vfs (native directory required)
    std::filesystem::path directory;           // used when non-empty (overrides directoryUri)
    u32 version = 1;                           // current save data version
    std::string gameVersion = "0.0.0";
    std::string extension = ".oxsave";
    // Autosave.
    f64 autosaveInterval = 0.0;                // seconds of play time, 0 = off
    bool autosaveOnLevelChange = true;
    u32 maxAutosaves = 3;                      // rotating "autosave0".."autosaveN-1"
    std::string quickSlot = "quicksave";
};

struct SaveResult {
    std::string slot;
    std::filesystem::path path;
    u64 bytes = 0;
};

struct LoadResult {
    SaveGameHeader header;
    bool fromBackup = false;       // main file was corrupt/missing and the backup was used
    u32 migratedFrom = 0;          // file data version before migrations
    usize entitiesCreated = 0;
    usize entitiesDestroyed = 0;
    usize entitiesUpdated = 0;
};

// Asynchronous operation handle (shared between the caller and the job).
template <class T>
class SaveOperation {
public:
    [[nodiscard]] bool done() const { return m_done.load(std::memory_order_acquire); }
    void wait() const {
        std::unique_lock lock(m_mutex);
        m_cv.wait(lock, [&] { return m_done.load(std::memory_order_acquire); });
    }
    // Valid once done().
    [[nodiscard]] const Result<T>& result() const {
        wait();
        return *m_result;
    }

    // Internal.
    void finish(Result<T> r) {
        {
            std::lock_guard lock(m_mutex);
            m_result.emplace(std::move(r));
            m_done.store(true, std::memory_order_release);
        }
        m_cv.notify_all();
    }

private:
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_cv;
    std::atomic<bool> m_done{false};
    std::optional<Result<T>> m_result;
};
using SaveHandle = std::shared_ptr<SaveOperation<SaveResult>>;
using LoadHandle = std::shared_ptr<SaveOperation<LoadResult>>;

// Failure injection for tests: return false to make the write fail at that stage.
enum class SaveWriteStage : u8 { WriteTemp, Fsync, BackupRename, CommitRename };

class SaveGameSystem {
public:
    using Migration = std::function<Status(serial::Document& doc)>;
    using ThumbnailProvider = std::function<std::vector<std::byte>()>;
    using WorldProvider = std::function<World*()>;
    // Loads the level for a save (fresh world) and returns the world to apply the save to; null = failure.
    using LevelLoader = std::function<World*(const std::string& levelUri)>;
    using FaultHook = std::function<bool(SaveWriteStage stage, std::span<const std::byte>& bytes)>;

    // jobs/vfs may be null (then everything runs synchronously / config.directory must be set).
    SaveGameSystem(SaveGameConfig config, JobSystem* jobs = nullptr, Vfs* vfs = nullptr);
    ~SaveGameSystem();
    SaveGameSystem(const SaveGameSystem&) = delete;
    SaveGameSystem& operator=(const SaveGameSystem&) = delete;

    [[nodiscard]] const SaveGameConfig& config() const { return m_config; }
    void setAutosaveInterval(f64 seconds) { m_config.autosaveInterval = seconds; }
    [[nodiscard]] const std::filesystem::path& directory() const { return m_dir; }

    // ---- hooks ----
    void setWorldProvider(WorldProvider fn) { m_worldProvider = std::move(fn); }
    void setLevelLoader(LevelLoader fn) { m_levelLoader = std::move(fn); }
    void setThumbnailProvider(ThumbnailProvider fn) { m_thumbnail = std::move(fn); }
    void setCurrentLevel(std::string levelUri) { m_level = std::move(levelUri); }
    [[nodiscard]] const std::string& currentLevel() const { return m_level; }
    // Remembers the entity UUIDs of a freshly loaded level so destroyed level entities can be recorded.
    void trackLevelEntities(const World& world);
    void setFaultHook(FaultHook fn) { m_faultHook = std::move(fn); }

    // ---- sections and migrations ----
    void registerSaveable(ISaveable& saveable);
    void unregisterSaveable(ISaveable& saveable);
    // Upgrades documents of data version `fromVersion` to fromVersion + 1 (operates on the value tree).
    void registerMigration(u32 fromVersion, Migration fn);

    // ---- synchronous API (game thread) ----
    Result<SaveResult> save(std::string_view slot, const World& world, SaveKind kind = SaveKind::Manual,
                            std::string displayName = {});
    // Loads and applies to `world` (no level loading).
    Result<LoadResult> load(std::string_view slot, World& world);
    // Loads the save's level through the LevelLoader (if set), then applies; falls back to the world provider.
    Result<LoadResult> load(std::string_view slot);

    // ---- asynchronous API ----
    // Snapshot on the calling (game) thread, encoding + disk I/O on the job system. The world may keep changing
    // right after the call returns.
    SaveHandle saveAsync(std::string_view slot, const World& world, SaveKind kind = SaveKind::Manual,
                         std::string displayName = {});
    // Reading/decoding/migration in the background; the world is modified in update() on the game thread.
    LoadHandle loadAsync(std::string_view slot);
    // Game thread, once per frame: finishes async loads, advances play time and autosave timers.
    void update(f64 realDt, bool playing = true);
    // Blocks until all async operations finished (and applies pending loads).
    void waitIdle();
    [[nodiscard]] usize pendingOperations() const;

    // ---- convenience ----
    Result<SaveResult> quickSave();
    Result<LoadResult> quickLoad();
    // Writes the next rotating autosave slot (uses the world provider).
    SaveHandle autosave();
    void onLevelChanged(const std::string& newLevel);

    // ---- slots ----
    [[nodiscard]] std::vector<SaveSlotInfo> listSlots() const; // newest first
    [[nodiscard]] Result<SaveSlotInfo> slotInfo(std::string_view slot) const;
    [[nodiscard]] Result<std::vector<std::byte>> thumbnail(std::string_view slot) const;
    [[nodiscard]] bool exists(std::string_view slot) const;
    bool deleteSlot(std::string_view slot);
    // Pretty JSON of the save (same content as `oxdump <slot>.oxsave`); written next to it when `outPath` given.
    Result<std::string> exportJson(std::string_view slot, const std::filesystem::path& outPath = {}) const;
    [[nodiscard]] std::filesystem::path slotPath(std::string_view slot) const;
    [[nodiscard]] static bool isValidSlotName(std::string_view slot);

    [[nodiscard]] f64 playTime() const { return m_playTime; }
    void setPlayTime(f64 seconds) { m_playTime = seconds; }
    [[nodiscard]] std::vector<std::string> autosaveSlots() const;

    Signal<const SaveResult&> saved;
    Signal<const LoadResult&> loaded;
    Signal<const std::string&> failed; // message

    // ---- building blocks (exposed for tools/tests) ----
    [[nodiscard]] serial::Document snapshot(std::string_view slot, const World& world, SaveKind kind,
                                            std::string displayName) const;
    Status writeDocument(std::string_view slot, const serial::Document& doc, u64* bytesOut = nullptr);
    // Reads main file, falls back to the backup when missing/corrupt, applies migrations.
    Result<serial::Document> readDocument(std::string_view slot, bool* fromBackup = nullptr,
                                          u32* migratedFrom = nullptr) const;
    Result<LoadResult> apply(const serial::Document& doc, World& world);
    Status migrate(serial::Document& doc) const;

private:
    struct LoadState {
        std::atomic<bool> ready{false};
        std::optional<Result<serial::Document>> doc;
        bool fromBackup = false;
        u32 migratedFrom = 0;
    };
    struct PendingLoad {
        LoadHandle handle;
        std::shared_ptr<LoadState> state;
    };
    struct PendingSave {
        SaveHandle handle;
    };
    void runJob(std::function<void()> fn);
    std::mutex& slotMutex(const std::string& slot);
    Result<LoadResult> finishLoad(Result<serial::Document> doc, bool fromBackup, u32 migratedFrom);
    std::string nextAutosaveSlot();

    SaveGameConfig m_config;
    JobSystem* m_jobs;
    Vfs* m_vfs;
    std::filesystem::path m_dir;

    WorldProvider m_worldProvider;
    LevelLoader m_levelLoader;
    ThumbnailProvider m_thumbnail;
    FaultHook m_faultHook;
    std::string m_level;
    std::unordered_set<Uuid> m_levelEntities;

    std::vector<ISaveable*> m_saveables;
    std::map<u32, Migration> m_migrations;

    f64 m_playTime = 0.0;
    f64 m_sinceAutosave = 0.0;

    mutable std::mutex m_slotMutexesGuard;
    std::map<std::string, std::unique_ptr<std::mutex>> m_slotMutexes;

    mutable std::mutex m_opsMutex;
    std::vector<JobHandle> m_jobHandles;     // guarded by m_opsMutex
    std::vector<PendingLoad> m_pendingLoads; // game thread only
    std::vector<PendingSave> m_pendingSaves; // game thread only (signals emitted from update())
    u32 m_autosaveCursor = 0;
    bool m_autosaveCursorValid = false;
};

void registerSaveGameTypes();

} // namespace ox
