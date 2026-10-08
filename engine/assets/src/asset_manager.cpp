#include <oxwald/assets/asset_manager.hpp>
#include <oxwald/assets/material.hpp>
#include <oxwald/assets/mesh.hpp>
#include <oxwald/assets/texture.hpp>
#include <oxwald/core/jobs.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/core/profile.hpp>
#include <oxwald/core/serial/format.hpp>

#include <algorithm>
#include <chrono>

namespace ox::assets {

// ---- handles ------------------------------------------------------------------------------------------------

namespace detail {
void addRef(AssetSlot& slot) { slot.refs.fetch_add(1, std::memory_order_relaxed); }
void release(AssetSlot& slot) {
    if (slot.refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        slot.releasedAt.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
    }
}
} // namespace detail

UntypedAssetHandle::UntypedAssetHandle(std::shared_ptr<detail::AssetSlot> slot) : m_slot(std::move(slot)) {
    if (m_slot) detail::addRef(*m_slot);
}
UntypedAssetHandle::UntypedAssetHandle(const UntypedAssetHandle& o) : m_slot(o.m_slot) {
    if (m_slot) detail::addRef(*m_slot);
}
UntypedAssetHandle::UntypedAssetHandle(UntypedAssetHandle&& o) noexcept : m_slot(std::move(o.m_slot)) {}
UntypedAssetHandle& UntypedAssetHandle::operator=(const UntypedAssetHandle& o) {
    if (this != &o) {
        if (o.m_slot) detail::addRef(*o.m_slot);
        if (m_slot) detail::release(*m_slot);
        m_slot = o.m_slot;
    }
    return *this;
}
UntypedAssetHandle& UntypedAssetHandle::operator=(UntypedAssetHandle&& o) noexcept {
    if (this != &o) {
        if (m_slot) detail::release(*m_slot);
        m_slot = std::move(o.m_slot);
    }
    return *this;
}
UntypedAssetHandle::~UntypedAssetHandle() {
    if (m_slot) detail::release(*m_slot);
}

std::string UntypedAssetHandle::error() const {
    if (!m_slot) return "invalid handle";
    std::lock_guard lock(m_slot->mutex);
    return m_slot->error;
}

std::shared_future<bool> UntypedAssetHandle::future() const {
    if (!m_slot) {
        std::promise<bool> p;
        p.set_value(false);
        return p.get_future().share();
    }
    std::lock_guard lock(m_slot->mutex);
    return m_slot->future;
}

bool UntypedAssetHandle::wait() const {
    if (!m_slot) return false;
    auto f = future();
    if (f.valid()) f.wait();
    return isLoaded();
}

void UntypedAssetHandle::onLoaded(std::function<void(bool)> callback) const {
    if (!m_slot) {
        callback(false);
        return;
    }
    {
        std::lock_guard lock(m_slot->mutex);
        const AssetState s = m_slot->state.load();
        if (s != AssetState::Loaded && s != AssetState::Failed) {
            m_slot->callbacks.push_back(std::move(callback));
            return;
        }
    }
    callback(isLoaded());
}

std::shared_ptr<const void> UntypedAssetHandle::dataUntyped() const {
    if (!m_slot) return nullptr;
    std::lock_guard lock(m_slot->mutex);
    return m_slot->data;
}

// ---- manager --------------------------------------------------------------------------------------------------

namespace {
template <class T>
std::shared_ptr<const void> share(T&& v) {
    return std::make_shared<const std::decay_t<T>>(std::forward<T>(v));
}

template <class T>
AssetManager::Loader blobLoader() {
    return [](std::vector<std::byte> bytes, const AssetRecord&) -> Result<LoadedAsset> {
        auto blob = deserializeBlob(bytes);
        if (!blob) return blob.error();
        T out;
        static_cast<BlobAsset&>(out) = std::move(*blob);
        const usize mem = out.memoryUsage();
        return LoadedAsset{share(std::move(out)), mem, {}};
    };
}
} // namespace

AssetManager::AssetManager(IAssetSource& source, JobSystem* jobs) : AssetManager(source, jobs, Options{}) {}

AssetManager::AssetManager(IAssetSource& source, JobSystem* jobs, Options options)
    : m_source(source), m_jobs(jobs), m_options(options), m_mainThread(std::this_thread::get_id()) {
    registerAssetTypes();
    registerBuiltins();
    m_sourceConnection = m_source.subscribeChanges([this](const Uuid& id) {
        std::lock_guard lock(m_mutex);
        if (m_slots.count(id)) m_reloadRequests.push_back(id);
    });
}

AssetManager::~AssetManager() {
    m_sourceConnection.disconnect();
    std::unique_lock lock(m_inflightMutex);
    m_inflightCv.wait(lock, [&] { return m_inflight == 0; });
}

void AssetManager::registerBuiltins() {
    registerLoader(AssetType::Mesh, [](std::vector<std::byte> bytes, const AssetRecord&) -> Result<LoadedAsset> {
        auto mesh = deserializeMesh(bytes);
        if (!mesh) return mesh.error();
        const usize mem = mesh->memoryUsage();
        return LoadedAsset{share(std::move(*mesh)), mem, {}};
    });
    registerLoader(AssetType::Texture, [](std::vector<std::byte> bytes, const AssetRecord&) -> Result<LoadedAsset> {
        auto tex = deserializeTexture(bytes);
        if (!tex) return tex.error();
        const usize mem = tex->memoryUsage();
        return LoadedAsset{share(std::move(*tex)), mem, {}};
    });
    registerLoader(AssetType::Material, [](std::vector<std::byte> bytes, const AssetRecord&) -> Result<LoadedAsset> {
        auto mat = loadMaterial(bytes);
        if (!mat) return mat.error();
        auto deps = mat->textureDependencies();
        return LoadedAsset{share(std::move(*mat)), sizeof(MaterialAsset), std::move(deps)};
    });
    registerLoader(AssetType::Scene, [](std::vector<std::byte> bytes, const AssetRecord&) -> Result<LoadedAsset> {
        auto doc = serial::decodeAny(bytes);
        if (!doc) return doc.error();
        SceneAsset s{std::move(*doc)};
        const usize mem = s.memoryUsage();
        return LoadedAsset{share(std::move(s)), mem, {}};
    });
    registerLoader(AssetType::Prefab, [](std::vector<std::byte> bytes, const AssetRecord&) -> Result<LoadedAsset> {
        auto doc = serial::decodeAny(bytes);
        if (!doc) return doc.error();
        PrefabAsset p{std::move(*doc)};
        const usize mem = p.memoryUsage();
        return LoadedAsset{share(std::move(p)), mem, {}};
    });
    registerLoader(AssetType::Script, [](std::vector<std::byte> bytes, const AssetRecord& rec) -> Result<LoadedAsset> {
        ScriptAsset s{rec.path, std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size())};
        const usize mem = s.memoryUsage();
        return LoadedAsset{share(std::move(s)), mem, {}};
    });
    registerLoader(AssetType::Audio, blobLoader<AudioClipAsset>());
    registerLoader(AssetType::Font, blobLoader<FontAsset>());
    registerLoader(AssetType::NavMesh, blobLoader<NavMeshAsset>());
    registerLoader(AssetType::Heightmap, blobLoader<HeightmapAsset>());
    registerLoader(AssetType::Raw, blobLoader<RawAsset>());
#if OX_ASSETS_HAS_ANIMATION
    registerLoader(AssetType::Skeleton, [](std::vector<std::byte> bytes, const AssetRecord&) -> Result<LoadedAsset> {
        auto s = deserializeSkeletonAsset(bytes);
        if (!s) return s.error();
        const usize mem = s->memoryUsage();
        return LoadedAsset{share(std::move(*s)), mem, {}};
    });
    registerLoader(AssetType::AnimationClip, [](std::vector<std::byte> bytes, const AssetRecord&) -> Result<LoadedAsset> {
        auto c = deserializeClipAsset(bytes);
        if (!c) return c.error();
        const usize mem = c->memoryUsage();
        std::vector<Uuid> deps;
        if (c->skeleton.isValid()) deps.push_back(c->skeleton);
        return LoadedAsset{share(std::move(*c)), mem, std::move(deps)};
    });
#endif

    auto checker = share(makeCheckerTexture());
    auto cube = share(makeCubeMesh());
    auto material = share(MaterialAsset{});
    setPlaceholder(AssetType::Texture, checker);
    setPlaceholder(AssetType::Mesh, cube);
    setPlaceholder(AssetType::Material, material);
    installBuiltin(builtin::checkerTexture(), AssetType::Texture, checker, 64 * 64 * 4);
    installBuiltin(builtin::whiteTexture(), AssetType::Texture, share(makeSolidTexture(0xffffffffu, true)), 128);
    installBuiltin(builtin::flatNormalTexture(), AssetType::Texture, share(makeSolidTexture(0xffff8080u, false)), 128);
    installBuiltin(builtin::cubeMesh(), AssetType::Mesh, cube, static_cast<const MeshData*>(cube.get())->memoryUsage());
    installBuiltin(builtin::defaultMaterial(), AssetType::Material, material, sizeof(MaterialAsset));
}

void AssetManager::installBuiltin(const Uuid& uuid, AssetType type, std::shared_ptr<const void> data, usize memory) {
    auto slot = std::make_shared<detail::AssetSlot>();
    slot->uuid = uuid;
    slot->type = type;
    slot->pinned = true;
    slot->data = std::move(data);
    slot->placeholder = placeholderFor(type);
    slot->memory = memory;
    slot->state = AssetState::Loaded;
    slot->generation = 1;
    auto promise = std::make_shared<std::promise<bool>>();
    promise->set_value(true);
    slot->promise = promise;
    slot->future = promise->get_future().share();
    std::lock_guard lock(m_mutex);
    m_slots[uuid] = std::move(slot);
}

void AssetManager::registerLoader(AssetType type, Loader loader) {
    std::lock_guard lock(m_mutex);
    m_loaders[type] = std::move(loader);
}

void AssetManager::setPlaceholder(AssetType type, std::shared_ptr<const void> data) {
    std::lock_guard lock(m_mutex);
    m_placeholders[type] = std::move(data);
}

std::shared_ptr<const void> AssetManager::placeholderFor(AssetType type) const {
    std::lock_guard lock(m_mutex);
    auto it = m_placeholders.find(type);
    return it == m_placeholders.end() ? nullptr : it->second;
}

AssetManager::SlotPtr AssetManager::getOrCreateSlot(const Uuid& uuid, AssetType type) {
    auto it = m_slots.find(uuid);
    if (it != m_slots.end()) return it->second;
    auto slot = std::make_shared<detail::AssetSlot>();
    slot->uuid = uuid;
    slot->type = type;
    if (auto p = m_placeholders.find(type); p != m_placeholders.end()) slot->placeholder = p->second;
    m_slots[uuid] = slot;
    return slot;
}

UntypedAssetHandle AssetManager::find(const Uuid& uuid) {
    std::lock_guard lock(m_mutex);
    auto it = m_slots.find(uuid);
    return it == m_slots.end() ? UntypedAssetHandle{} : UntypedAssetHandle(it->second);
}

AssetState AssetManager::state(const Uuid& uuid) {
    std::lock_guard lock(m_mutex);
    auto it = m_slots.find(uuid);
    return it == m_slots.end() ? AssetState::Unloaded : it->second->state.load();
}

UntypedAssetHandle AssetManager::loadUntyped(const Uuid& uuid, AssetType expectedType, i32 priority, bool synchronous) {
    OX_PROFILE_ZONE();
    if (uuid.isNil()) {
        auto slot = std::make_shared<detail::AssetSlot>();
        slot->type = expectedType;
        slot->placeholder = placeholderFor(expectedType);
        slot->error = "nil asset id";
        slot->state = AssetState::Failed;
        auto p = std::make_shared<std::promise<bool>>();
        p->set_value(false);
        slot->future = p->get_future().share();
        return UntypedAssetHandle(slot);
    }
    SlotPtr slot;
    UntypedAssetHandle handle;
    bool start = false;
    bool requeue = false;
    {
        std::lock_guard lock(m_mutex);
        slot = getOrCreateSlot(uuid, expectedType);
        handle = UntypedAssetHandle(slot); // ref taken under the lock: eviction cannot race
        std::lock_guard slotLock(slot->mutex);
        if (slot->state.load() == AssetState::Unloaded) {
            slot->state = AssetState::Queued;
            slot->priority = priority;
            slot->error.clear();
            slot->promise = std::make_shared<std::promise<bool>>();
            slot->future = slot->promise->get_future().share();
            start = true;
        } else if (slot->state.load() == AssetState::Queued && priority > slot->priority) {
            slot->priority = priority; // re-queued with the higher priority; the stale entry finds it claimed
            requeue = m_jobs && !synchronous;
        }
    }
    if (requeue) enqueue(slot, priority, false);
    if (expectedType != AssetType::Unknown && slot->type != AssetType::Unknown && slot->type != expectedType) {
        OX_LOG_WARN("assets", "asset {} requested as {} but is {}", uuid.toString(), assetTypeName(expectedType),
                    assetTypeName(slot->type));
    }
    if (start) {
        if (synchronous || !m_jobs) {
            slot->state = AssetState::Loading;
            execute(slot, false, true);
        } else {
            enqueue(slot, priority, false);
        }
    }
    if (synchronous) handle.wait();
    return handle;
}

void AssetManager::enqueue(const SlotPtr& slot, i32 priority, bool reload) {
    {
        std::lock_guard lock(m_mutex);
        m_queue.push(Request{priority, m_sequence++, slot, reload});
    }
    {
        std::lock_guard lock(m_inflightMutex);
        ++m_inflight;
    }
    m_jobs->submit([this] { pump(); });
}

void AssetManager::pump() {
    Request req{};
    bool have = false;
    {
        std::lock_guard lock(m_mutex);
        if (!m_queue.empty()) {
            req = m_queue.top();
            m_queue.pop();
            have = true;
        }
    }
    if (have) {
        if (req.reload) {
            execute(req.slot, true, false);
        } else {
            // Duplicate queue entries (priority bumps) find the slot already claimed.
            AssetState expected = AssetState::Queued;
            if (req.slot->state.compare_exchange_strong(expected, AssetState::Loading)) execute(req.slot, false, false);
        }
    }
    {
        std::lock_guard lock(m_inflightMutex);
        --m_inflight;
    }
    m_inflightCv.notify_all();
}

void AssetManager::execute(const SlotPtr& slot, bool reload, bool synchronous) {
    OX_PROFILE_ZONE();
    auto fail = [&](std::string message) {
        OX_LOG_ERROR("assets", "loading {} failed: {}", slot->uuid.toString(), message);
        if (reload) {
            postEvent(Event{Event::Failed, slot->uuid, slot->type, message, {}, false});
            return;
        }
        {
            std::lock_guard lock(slot->mutex);
            slot->error = std::move(message);
        }
        finish(slot, false);
    };
    auto rec = m_source.record(slot->uuid);
    if (!rec) return fail("unknown asset or import failed");
    if (slot->type != AssetType::Unknown && rec->type != slot->type) {
        return fail(std::format("type mismatch: requested {}, asset is {}", assetTypeName(slot->type),
                                assetTypeName(rec->type)));
    }
    if (slot->type == AssetType::Unknown) {
        slot->type = rec->type;
        auto ph = placeholderFor(rec->type);
        std::lock_guard lock(slot->mutex);
        slot->placeholder = ph;
    }
    Loader loader;
    {
        std::lock_guard lock(m_mutex);
        if (auto it = m_loaders.find(rec->type); it != m_loaders.end()) loader = it->second;
    }
    if (!loader) return fail(std::format("no loader for type {}", assetTypeName(rec->type)));
    auto bytes = m_source.readArtifact(slot->uuid);
    if (!bytes) return fail(bytes.error().message);
    Result<LoadedAsset> loaded = [&]() -> Result<LoadedAsset> {
        try {
            return loader(std::move(*bytes), *rec);
        } catch (const std::exception& e) {
            return makeError("loader threw: {}", e.what());
        }
    }();
    if (!loaded) return fail(loaded.error().message);

    std::vector<Uuid> deps = rec->dependencies;
    for (const Uuid& d : loaded->dependencies) {
        if (std::find(deps.begin(), deps.end(), d) == deps.end()) deps.push_back(d);
    }
    std::erase(deps, slot->uuid);
    std::vector<SlotPtr> depSlots;
    for (const Uuid& d : deps) {
        UntypedAssetHandle h = loadUntyped(d, AssetType::Unknown, slot->priority, synchronous);
        detail::addRef(*h.slot()); // kept while this asset is loaded
        depSlots.push_back(h.slot());
    }

    if (reload) {
        for (const auto& d : depSlots) {
            std::shared_future<bool> f;
            {
                std::lock_guard lock(d->mutex);
                f = d->future;
            }
            if (f.valid()) f.wait();
        }
        std::lock_guard lock(m_mutex);
        m_reloadsReady.push_back(PendingReload{slot, std::move(*loaded), std::move(depSlots)});
        return;
    }

    slot->pendingDependencies.store(1);
    {
        std::lock_guard lock(slot->mutex);
        slot->staged = std::move(loaded->data);
        slot->stagedMemory = loaded->memory;
        slot->dependencies = depSlots;
        slot->state = AssetState::WaitingForDependencies;
    }
    for (const auto& d : depSlots) {
        std::lock_guard lock(d->mutex);
        const AssetState s = d->state.load();
        if (s != AssetState::Loaded && s != AssetState::Failed) {
            d->waiters.push_back(slot);
            slot->pendingDependencies.fetch_add(1);
        }
    }
    dependencyDone(slot);
}

void AssetManager::dependencyDone(const SlotPtr& waiter) {
    if (waiter->pendingDependencies.fetch_sub(1) == 1) finish(waiter, true);
}

void AssetManager::finish(const SlotPtr& slot, bool ok) {
    std::vector<std::function<void(bool)>> callbacks;
    std::vector<SlotPtr> waiters;
    std::shared_ptr<std::promise<bool>> promise;
    std::string error;
    {
        std::lock_guard lock(slot->mutex);
        if (ok) {
            slot->data = std::move(slot->staged);
            slot->memory = slot->stagedMemory;
            slot->generation.fetch_add(1);
            slot->state = AssetState::Loaded;
        } else {
            slot->staged.reset();
            slot->state = AssetState::Failed;
            error = slot->error;
        }
        callbacks.swap(slot->callbacks);
        waiters.swap(slot->waiters);
        promise = slot->promise;
    }
    if (promise) promise->set_value(ok);
    postEvent(Event{ok ? Event::Loaded : Event::Failed, slot->uuid, slot->type, error, {}, ok});
    for (auto& cb : callbacks) {
        if (m_options.callbacksOnMainThread) postEvent(Event{Event::Callback, slot->uuid, slot->type, {}, std::move(cb), ok});
        else cb(ok);
    }
    for (const auto& w : waiters) dependencyDone(w);
}

void AssetManager::postEvent(Event e) {
    std::lock_guard lock(m_mutex);
    m_events.push_back(std::move(e));
}

void AssetManager::reload(const Uuid& uuid) {
    std::lock_guard lock(m_mutex);
    m_reloadRequests.push_back(uuid);
}

void AssetManager::update() {
    OX_PROFILE_ZONE();
    std::vector<Uuid> reloads;
    std::vector<PendingReload> ready;
    std::vector<Event> events;
    {
        std::lock_guard lock(m_mutex);
        m_graveyard.clear();
        reloads.swap(m_reloadRequests);
        ready.swap(m_reloadsReady);
    }
    std::sort(reloads.begin(), reloads.end());
    reloads.erase(std::unique(reloads.begin(), reloads.end()), reloads.end());
    for (const Uuid& id : reloads) {
        SlotPtr slot;
        {
            std::lock_guard lock(m_mutex);
            auto it = m_slots.find(id);
            if (it == m_slots.end() || it->second->pinned) continue;
            slot = it->second;
        }
        const AssetState s = slot->state.load();
        if (s == AssetState::Failed) {
            // Retry failed assets on change: back to Unloaded and load again with the old priority.
            {
                std::lock_guard lock(slot->mutex);
                slot->state = AssetState::Unloaded;
            }
            loadUntyped(id, slot->type, slot->priority, !m_jobs);
            continue;
        }
        if (s != AssetState::Loaded) continue; // in-flight loads read the new data anyway
        if (m_jobs) enqueue(slot, slot->priority, true);
        else execute(slot, true, true);
    }
    if (!m_jobs) {
        std::lock_guard lock(m_mutex);
        for (auto& r : m_reloadsReady) ready.push_back(std::move(r));
        m_reloadsReady.clear();
    }
    for (auto& r : ready) {
        std::vector<SlotPtr> oldDeps;
        std::shared_ptr<const void> oldData;
        {
            std::lock_guard lock(r.slot->mutex);
            oldData = std::move(r.slot->data);
            r.slot->data = std::move(r.asset.data);
            r.slot->memory = r.asset.memory;
            oldDeps.swap(r.slot->dependencies);
            r.slot->dependencies = std::move(r.dependencies);
            r.slot->generation.fetch_add(1);
        }
        for (const auto& d : oldDeps) detail::release(*d);
        {
            std::lock_guard g(m_mutex);
            m_graveyard.push_back(std::move(oldData)); // raw pointers from get() stay valid until the next update()
        }
        postEvent(Event{Event::Reloaded, r.slot->uuid, r.slot->type, {}, {}, true});
    }
    {
        std::lock_guard lock(m_mutex);
        events.swap(m_events);
    }
    for (auto& e : events) {
        switch (e.kind) {
        case Event::Loaded: onLoaded.emit(e.uuid, e.type); break;
        case Event::Failed: onFailed.emit(e.uuid, e.error); break;
        case Event::Reloaded: onReloaded.emit(e.uuid, e.type); break;
        case Event::Callback:
            if (e.callback) e.callback(e.ok);
            break;
        }
    }
    std::vector<SlotPtr> evicted;
    {
        std::lock_guard lock(m_mutex);
        evictLocked(evicted, false);
    }
    for (const auto& s : evicted) onUnloaded.emit(s->uuid, s->type);
}

void AssetManager::evictLocked(std::vector<SlotPtr>& evicted, bool all) {
    for (bool progress = true; progress;) {
        progress = false;
        std::vector<SlotPtr> candidates;
        usize total = 0;
        for (const auto& [id, s] : m_slots) {
            const AssetState st = s->state.load();
            if (st == AssetState::Loaded) total += s->memory;
            if (s->pinned || s->refs.load() > 0) continue;
            if (st == AssetState::Loaded || st == AssetState::Failed) candidates.push_back(s);
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const SlotPtr& a, const SlotPtr& b) { return a->releasedAt.load() < b->releasedAt.load(); });
        usize remaining = candidates.size();
        for (const auto& s : candidates) {
            const bool overCount = remaining > m_options.keepUnreferenced;
            const bool overBudget = m_options.memoryBudget > 0 && total > m_options.memoryBudget;
            const bool failed = s->state.load() == AssetState::Failed;
            if (!all && !overCount && !overBudget && !failed) break;
            std::vector<SlotPtr> deps;
            {
                std::lock_guard lock(s->mutex);
                if (s->refs.load() > 0) continue;
                if (s->state.load() == AssetState::Loaded) total -= std::min(total, s->memory);
                m_graveyard.push_back(std::move(s->data));
                deps.swap(s->dependencies);
                s->state = AssetState::Unloaded;
            }
            for (const auto& d : deps) detail::release(*d);
            m_slots.erase(s->uuid);
            if (!failed) evicted.push_back(s);
            --remaining;
            progress = true;
        }
    }
}

usize AssetManager::unloadUnused() {
    std::vector<SlotPtr> evicted;
    {
        std::lock_guard lock(m_mutex);
        evictLocked(evicted, true);
    }
    for (const auto& s : evicted) onUnloaded.emit(s->uuid, s->type);
    return evicted.size();
}

void AssetManager::waitAll() {
    for (int guard = 0; guard < 1000; ++guard) {
        {
            std::unique_lock lock(m_inflightMutex);
            m_inflightCv.wait(lock, [&] { return m_inflight == 0; });
        }
        update();
        std::lock_guard lock(m_mutex);
        std::lock_guard inflight(m_inflightMutex);
        if (m_inflight == 0 && m_reloadRequests.empty() && m_reloadsReady.empty() && m_queue.empty()) break;
    }
}

void AssetManager::setMemoryBudget(usize bytes) {
    std::lock_guard lock(m_mutex);
    m_options.memoryBudget = bytes;
}

AssetStats AssetManager::stats() const {
    std::lock_guard lock(m_mutex);
    AssetStats s;
    s.memoryBudget = m_options.memoryBudget;
    for (const auto& [id, slot] : m_slots) {
        switch (slot->state.load()) {
        case AssetState::Loaded: {
            ++s.loaded;
            s.memoryBytes += slot->memory;
            auto& t = s.perType[slot->type];
            ++t.count;
            t.bytes += slot->memory;
            if (slot->refs.load() == 0 && !slot->pinned) ++s.unreferenced;
            break;
        }
        case AssetState::Failed: ++s.failed; break;
        case AssetState::Unloaded: break;
        default: ++s.loading; break;
        }
    }
    return s;
}

} // namespace ox::assets
