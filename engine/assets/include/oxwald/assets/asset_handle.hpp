#pragma once

#include <oxwald/assets/asset_types.hpp>

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ox::assets {

class AssetManager;

enum class AssetState : u8 { Unloaded, Queued, Loading, WaitingForDependencies, Loaded, Failed };

namespace detail {
struct AssetSlot {
    Uuid uuid;
    AssetType type = AssetType::Unknown;
    std::atomic<AssetState> state{AssetState::Unloaded};
    std::atomic<i32> refs{0};
    std::atomic<i64> releasedAt{0}; // steady clock ticks when refs dropped to 0 (LRU)
    std::atomic<u32> generation{0}; // incremented by every successful (re)load
    std::atomic<u32> pendingDependencies{0};
    i32 priority = 0;
    bool pinned = false; // built-ins

    mutable std::mutex mutex; // guards everything below
    std::shared_ptr<const void> data;
    std::shared_ptr<const void> placeholder;
    std::shared_ptr<const void> staged; // loaded, waiting for dependencies
    usize memory = 0;
    usize stagedMemory = 0;
    std::string error;
    std::vector<std::function<void(bool)>> callbacks;
    std::vector<std::shared_ptr<AssetSlot>> waiters;      // slots waiting on this one
    std::vector<std::shared_ptr<AssetSlot>> dependencies; // referenced (refs held) while loaded
    std::shared_ptr<std::promise<bool>> promise;
    std::shared_future<bool> future;
};
void addRef(AssetSlot& slot);
void release(AssetSlot& slot);
} // namespace detail

// Ref-counted reference to an asset. Copies share the load; when the last handle of an asset is dropped it
// becomes eligible for unloading (AssetManager keeps an LRU of unreferenced assets, see Options).
class UntypedAssetHandle {
public:
    UntypedAssetHandle() = default;
    explicit UntypedAssetHandle(std::shared_ptr<detail::AssetSlot> slot);
    UntypedAssetHandle(const UntypedAssetHandle& o);
    UntypedAssetHandle(UntypedAssetHandle&& o) noexcept;
    UntypedAssetHandle& operator=(const UntypedAssetHandle& o);
    UntypedAssetHandle& operator=(UntypedAssetHandle&& o) noexcept;
    ~UntypedAssetHandle();

    [[nodiscard]] bool valid() const { return m_slot != nullptr; }
    explicit operator bool() const { return valid(); }
    [[nodiscard]] Uuid uuid() const { return m_slot ? m_slot->uuid : Uuid{}; }
    [[nodiscard]] AssetType type() const { return m_slot ? m_slot->type : AssetType::Unknown; }
    [[nodiscard]] AssetState state() const { return m_slot ? m_slot->state.load() : AssetState::Unloaded; }
    [[nodiscard]] bool isLoaded() const { return state() == AssetState::Loaded; }
    [[nodiscard]] bool isFailed() const { return state() == AssetState::Failed; }
    // Loaded or failed.
    [[nodiscard]] bool isDone() const { return isLoaded() || isFailed(); }
    [[nodiscard]] std::string error() const;
    // Changes on every hot reload (GPU caches compare it to detect stale uploads).
    [[nodiscard]] u32 generation() const { return m_slot ? m_slot->generation.load() : 0; }
    // Resolves to true when loaded (incl. dependencies), false on failure.
    [[nodiscard]] std::shared_future<bool> future() const;
    // Blocks until done; returns isLoaded().
    bool wait() const;
    // Called once when loading finishes (true = loaded). Already done: called immediately on this thread.
    // Otherwise on the main thread in AssetManager::update() (or on the loading thread, see Options).
    void onLoaded(std::function<void(bool)> callback) const;

    [[nodiscard]] std::shared_ptr<const void> dataUntyped() const;
    [[nodiscard]] const std::shared_ptr<detail::AssetSlot>& slot() const { return m_slot; }

    friend bool operator==(const UntypedAssetHandle& a, const UntypedAssetHandle& b) { return a.m_slot == b.m_slot; }

protected:
    std::shared_ptr<detail::AssetSlot> m_slot;
};

template <class T>
class AssetHandle : public UntypedAssetHandle {
public:
    AssetHandle() = default;
    explicit AssetHandle(UntypedAssetHandle h) : UntypedAssetHandle(std::move(h)) {}

    // nullptr until loaded. The pointer stays valid until the next AssetManager::update() after a hot reload or
    // unload; keep share() for longer-lived access.
    [[nodiscard]] const T* get() const { return static_cast<const T*>(dataUntyped().get()); }
    [[nodiscard]] std::shared_ptr<const T> share() const { return std::static_pointer_cast<const T>(dataUntyped()); }
    // The loaded asset, or the built-in placeholder of the type (checker texture, cube, default material).
    [[nodiscard]] const T* getOrDefault() const {
        if (const T* p = get()) return p;
        if (!m_slot) return nullptr;
        std::lock_guard lock(m_slot->mutex);
        return static_cast<const T*>(m_slot->placeholder.get());
    }
    const T* operator->() const { return get(); }
};

} // namespace ox::assets
