#include <oxwald/core/log.hpp>
#include <oxwald/core/profile.hpp>
#include <oxwald/core/reflect.hpp>
#include <oxwald/core/serial/format.hpp>
#include <oxwald/core/vfs.hpp>
#include <oxwald/runtime/save_game.hpp>
#include <oxwald/scene/component_registry.hpp>
#include <oxwald/scene/scene.hpp>
#include <oxwald/scene/scene_serializer.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <set>
#include <unordered_map>

#if defined(_WIN32)
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace ox {

using serial::Tag;
using serial::TypeDesc;
using serial::Value;

namespace {

constexpr std::string_view kKind = "savegame";
constexpr std::string_view kEngineVersion = "0.1.0";

i64 unixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

bool hasSaveFields(const reflect::TypeInfo& t) {
    if (t.attributes.saveGame) return true;
    return std::any_of(t.fields.begin(), t.fields.end(), [](const reflect::FieldInfo& f) { return f.attributes.saveGame; });
}

// Only the component's own attr::SaveGame fields; nested values of such a field are kept whole.
serial::ConvertOptions saveFieldsOnly(const reflect::TypeInfo& t) {
    serial::ConvertOptions o;
    if (t.attributes.saveGame) return o;
    std::unordered_set<const reflect::FieldInfo*> top;
    for (const auto& f : t.fields) top.insert(&f);
    o.fieldFilter = [top = std::move(top)](const reflect::FieldInfo& f) {
        return !top.contains(&f) || f.attributes.saveGame;
    };
    return o;
}

Value entityRecord(const World& world, entt::entity e, bool full) {
    World& w = const_cast<World&>(world);
    const Entity entity{e, &w};
    Value rec = Value::makeObject("SaveEntity");
    rec.set("id", Value::makeUuid(entity.uuid()));
    rec.set("name", Value::makeString(full ? entity.name() : std::string()));
    const Entity parent = entity.parent();
    rec.set("parent", Value::makeEntityRef(full && parent.valid() ? parent.uuid() : Uuid{}));
    rec.set("full", Value::makeBool(full));

    Value comps = Value::makeMap(TypeDesc::of(Tag::Object));
    const auto* sg = world.registry().try_get<SaveGameComponent>(e);
    for (const ComponentInfo* info : ComponentRegistry::instance().all()) {
        if (!info->serializable || !info->has(world, e)) continue;
        if (full) {
            if (!sg->saveTransform && info->typeId == entt::type_hash<TransformComponent>::value()) continue;
            comps.fields().emplace_back(info->name, info->serialize(world, e));
        } else if (hasSaveFields(*info->type)) {
            comps.fields().emplace_back(info->name, info->serialize(world, e, saveFieldsOnly(*info->type)));
        }
    }
    if (full) {
        if (const auto* unknown = world.registry().try_get<UnknownComponents>(e)) {
            for (const auto& [name, v] : unknown->entries) {
                if (!comps.find(name)) comps.fields().emplace_back(name, v);
            }
        }
    }
    rec.set("components", std::move(comps));
    return rec;
}

bool needsRecord(const World& world, entt::entity e, bool& full) {
    full = world.registry().all_of<SaveGameComponent>(e);
    if (full) return true;
    for (const ComponentInfo* info : ComponentRegistry::instance().all()) {
        if (info->serializable && hasSaveFields(*info->type) && info->has(world, e)) return true;
    }
    return false;
}

SaveGameHeader readHeader(const serial::Document& doc) {
    SaveGameHeader h;
    if (const Value* v = doc.root.find("header")) serial::fromValue(*v, h);
    return h;
}

Result<serial::Document> decodeSave(const std::filesystem::path& path) {
    auto bytes = serial::readFileBytes(path);
    if (!bytes) return bytes.error();
    auto doc = serial::decodeAny(*bytes);
    if (!doc) return makeError("{}: {}", path.filename().string(), doc.error().message);
    if (doc->kind != kKind) return makeError("{}: not a save game (kind '{}')", path.filename().string(), doc->kind);
    if (!doc->root.find("header")) return makeError("{}: save has no header", path.filename().string());
    return doc;
}

// POSIX write + fsync; `bytes` may have been shortened by the fault hook (simulated torn write).
Status writeAndSync(const std::filesystem::path& path, std::span<const std::byte> bytes, bool sync) {
#if defined(_WIN32)
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return makeError("cannot create '{}'", path.string());
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size() && std::fflush(f) == 0 &&
                    (!sync || _commit(_fileno(f)) == 0);
    std::fclose(f);
    if (!ok) return makeError("write failed for '{}'", path.string());
    return {};
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return makeError("cannot create '{}'", path.string());
    usize written = 0;
    while (written < bytes.size()) {
        const auto n = ::write(fd, bytes.data() + written, bytes.size() - written);
        if (n <= 0) {
            ::close(fd);
            return makeError("write failed for '{}'", path.string());
        }
        written += usize(n);
    }
    const bool ok = !sync || ::fsync(fd) == 0;
    ::close(fd);
    if (!ok) return makeError("fsync failed for '{}'", path.string());
    return {};
#endif
}

void syncDirectory([[maybe_unused]] const std::filesystem::path& dir) {
#if !defined(_WIN32)
    const int fd = ::open(dir.c_str(), O_RDONLY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
#endif
}

} // namespace

void registerSaveGameTypes() {
    registerSceneTypes();
    OX_REFLECT_ENUM(SaveKind, "SaveKind")
        .value("Manual", SaveKind::Manual)
        .value("Auto", SaveKind::Auto)
        .value("Quick", SaveKind::Quick);
    OX_REFLECT_TYPE(SaveGameHeader, "SaveGameHeader")
        .field("slot", &SaveGameHeader::slot)
        .field("displayName", &SaveGameHeader::displayName)
        .field("timestamp", &SaveGameHeader::timestamp)
        .field("playTimeSeconds", &SaveGameHeader::playTimeSeconds)
        .field("level", &SaveGameHeader::level)
        .field("gameVersion", &SaveGameHeader::gameVersion)
        .field("engineVersion", &SaveGameHeader::engineVersion)
        .field("dataVersion", &SaveGameHeader::dataVersion)
        .field("kind", &SaveGameHeader::kind)
        .field("entityCount", &SaveGameHeader::entityCount);
}

SaveGameSystem::SaveGameSystem(SaveGameConfig config, JobSystem* jobs, Vfs* vfs)
    : m_config(std::move(config)), m_jobs(jobs), m_vfs(vfs) {
    registerSaveGameTypes();
    if (!m_config.directory.empty()) {
        m_dir = m_config.directory;
    } else if (m_vfs) {
        if (auto p = m_vfs->resolveNative(m_config.directoryUri + "/.probe")) m_dir = p->parent_path();
    }
    if (m_dir.empty()) {
        OX_LOG_ERROR("save", "no native directory for '{}' — saving disabled", m_config.directoryUri);
    } else {
        std::error_code ec;
        std::filesystem::create_directories(m_dir, ec);
    }
}

SaveGameSystem::~SaveGameSystem() { waitIdle(); }

bool SaveGameSystem::isValidSlotName(std::string_view slot) {
    if (slot.empty() || slot.size() > 64) return false;
    return std::all_of(slot.begin(), slot.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

std::filesystem::path SaveGameSystem::slotPath(std::string_view slot) const {
    return m_dir / (std::string(slot) + m_config.extension);
}

void SaveGameSystem::trackLevelEntities(const World& world) {
    m_levelEntities.clear();
    for (auto [e, id] : world.registry().view<const IdComponent>().each()) m_levelEntities.insert(id.id);
}

void SaveGameSystem::registerSaveable(ISaveable& saveable) {
    if (std::find(m_saveables.begin(), m_saveables.end(), &saveable) == m_saveables.end()) {
        m_saveables.push_back(&saveable);
    }
}

void SaveGameSystem::unregisterSaveable(ISaveable& saveable) { std::erase(m_saveables, &saveable); }

void SaveGameSystem::registerMigration(u32 fromVersion, Migration fn) { m_migrations[fromVersion] = std::move(fn); }

// ---- snapshot / apply -------------------------------------------------------------------------------------------

serial::Document SaveGameSystem::snapshot(std::string_view slot, const World& world, SaveKind kind,
                                          std::string displayName) const {
    OX_PROFILE_ZONE();
    serial::Document doc;
    doc.kind = std::string(kKind);
    doc.version = m_config.version;
    doc.root = Value::makeObject("SaveGame");

    Value entities = Value::makeArray(TypeDesc::of(Tag::Object));
    std::unordered_set<Uuid> alive;
    world.forEachInHierarchy([&](entt::entity e) {
        alive.insert(world.registry().get<IdComponent>(e).id);
        bool full = false;
        if (needsRecord(world, e, full)) entities.push(entityRecord(world, e, full));
    });
    Value destroyed = Value::makeArray(TypeDesc::of(Tag::Uuid));
    std::vector<Uuid> gone;
    for (const Uuid& id : m_levelEntities) {
        if (!alive.contains(id)) gone.push_back(id);
    }
    std::sort(gone.begin(), gone.end(), [](const Uuid& a, const Uuid& b) { return a.toString() < b.toString(); });
    for (const Uuid& id : gone) destroyed.push(Value::makeUuid(id));

    SaveGameHeader header;
    header.slot = std::string(slot);
    header.displayName = displayName.empty() ? std::string(slot) : std::move(displayName);
    header.timestamp = unixNow();
    header.playTimeSeconds = m_playTime;
    header.level = m_level;
    header.gameVersion = m_config.gameVersion;
    header.engineVersion = std::string(kEngineVersion);
    header.dataVersion = m_config.version;
    header.kind = kind;
    header.entityCount = entities.size();
    doc.root.set("header", serial::toValue(header));

    std::vector<std::byte> thumb;
    if (m_thumbnail) thumb = m_thumbnail();
    doc.root.set("thumbnail", Value::makeRawArray(TypeDesc::of(Tag::U8), std::move(thumb)));

    Value worldValue = Value::makeObject("SaveWorld");
    worldValue.set("level", Value::makeString(m_level));
    worldValue.set("entities", std::move(entities));
    worldValue.set("destroyed", std::move(destroyed));
    doc.root.set("world", std::move(worldValue));

    Value sections = Value::makeMap(TypeDesc::of(Tag::Object));
    for (const ISaveable* s : m_saveables) {
        serial::Writer w("section", m_config.version);
        s->save(w);
        sections.set(s->saveId(), std::move(w).finish().root);
    }
    doc.root.set("sections", std::move(sections));
    return doc;
}

Result<LoadResult> SaveGameSystem::apply(const serial::Document& doc, World& world) {
    OX_PROFILE_ZONE();
    LoadResult result;
    result.header = readHeader(doc);
    const Value* worldValue = doc.root.find("world");
    const Value* entities = worldValue ? worldValue->find("entities") : nullptr;
    if (!entities || !entities->isArray()) return makeError("save: no world entities");

    struct Rec {
        const Value* value;
        Uuid id;
        bool full;
    };
    std::vector<Rec> records;
    std::unordered_set<Uuid> savedIds;
    for (const Value& v : entities->items()) {
        const Value* id = v.find("id");
        const Value* full = v.find("full");
        if (!id || !id->getUuid().isValid()) continue;
        records.push_back({&v, id->getUuid(), full && full->getBool()});
        savedIds.insert(id->getUuid());
    }

    // 1. Destroy entities that did not exist at save time: level entities recorded as destroyed and whole-entity
    //    saveables (SaveGameComponent) missing from the save (spawned after it was written).
    std::vector<Uuid> toDestroy;
    if (const Value* destroyed = worldValue->find("destroyed"); destroyed && destroyed->isArray()) {
        for (usize i = 0; i < destroyed->size(); ++i) toDestroy.push_back(destroyed->at(i).getUuid());
    }
    for (auto [e, id, sg] : world.registry().view<const IdComponent, const SaveGameComponent>().each()) {
        if (!savedIds.contains(id.id)) toDestroy.push_back(id.id);
    }
    for (const Uuid& id : toDestroy) {
        Entity e = world.find(id);
        if (!e.valid()) continue;
        world.destroyImmediate(e);
        result.entitiesDestroyed++;
    }

    // 2. Re-create whole entities that are missing (destroyed after the save, or never in the level). They keep
    //    their UUID, so EntityRefs stored anywhere (components, sections) resolve again.
    std::unordered_set<Uuid> created;
    for (const Rec& r : records) {
        if (!r.full || world.find(r.id).valid()) continue;
        const Value* name = r.value->find("name");
        world.createWithId(r.id, name ? name->getString() : std::string("Entity"));
        created.insert(r.id);
        result.entitiesCreated++;
    }

    // 3. Hierarchy and components (records are in hierarchy pre-order, so parents are restored first).
    const auto& registry = ComponentRegistry::instance();
    std::unordered_map<Uuid, Uuid> idMap; // saved id -> world id (identity; kept for remapEntityRefs)
    for (const Rec& r : records) idMap.emplace(r.id, r.id);
    for (const Rec& r : records) {
        Entity e = world.find(r.id);
        if (!e.valid()) {
            OX_LOG_WARN("save", "entity {} from the save is not in the level; its SaveGame fields are skipped",
                        r.id.toString());
            continue;
        }
        if (!created.contains(r.id)) result.entitiesUpdated++;
        const Value* comps = r.value->find("components");
        if (r.full) {
            if (const Value* name = r.value->find("name")) e.setName(name->getString());
            const Value* parentRef = r.value->find("parent");
            Entity parent = parentRef && parentRef->getUuid().isValid() ? world.find(parentRef->getUuid()) : Entity{};
            if (e.parent() != parent && !(parent.valid() && e.isAncestorOf(parent))) {
                world.setParent(e.handle(), parent.valid() ? parent.handle() : entt::null, false);
            }
            // Components added after the save are removed so the entity matches the snapshot exactly.
            for (const ComponentInfo* info : registry.componentsOf(world, e.handle())) {
                if (!info->serializable || !info->removable) continue;
                if (!comps || !comps->find(info->name)) info->remove(world, e.handle());
            }
            world.registry().remove<UnknownComponents>(e.handle());
        }
        if (!comps) continue;
        for (const auto& [compName, compValue] : comps->fields()) {
            Value v = compValue;
            remapEntityRefs(v, idMap);
            if (const ComponentInfo* info = registry.find(compName)) {
                if (!info->deserialize(world, e.handle(), v)) {
                    OX_LOG_WARN("save", "entity '{}': component {} has incompatible data", e.name(), compName);
                }
                info->notifyChanged(world, e.handle());
            } else if (r.full) {
                world.registry().get_or_emplace<UnknownComponents>(e.handle()).entries.emplace_back(compName, std::move(v));
            }
        }
        world.markTransformDirty(e.handle());
    }
    world.updateTransforms();
    world.snapshotPreviousTransforms();

    // 4. User sections.
    const Value* sections = doc.root.find("sections");
    for (ISaveable* s : m_saveables) {
        const Value* section = sections ? sections->find(s->saveId()) : nullptr;
        if (!section) {
            s->onMissing();
            continue;
        }
        serial::Document sdoc;
        sdoc.kind = "section";
        sdoc.version = doc.version;
        sdoc.root = *section;
        serial::Reader reader(std::move(sdoc));
        if (!s->load(reader)) OX_LOG_WARN("save", "section '{}' failed to load", s->saveId());
    }

    m_playTime = result.header.playTimeSeconds;
    if (!result.header.level.empty()) m_level = result.header.level;
    return result;
}

Status SaveGameSystem::migrate(serial::Document& doc) const {
    if (doc.version > m_config.version) {
        OX_LOG_WARN("save", "save data version {} is newer than this build ({}); loading what is understood",
                    doc.version, m_config.version);
        return {};
    }
    while (doc.version < m_config.version) {
        if (auto it = m_migrations.find(doc.version); it != m_migrations.end()) {
            if (auto st = it->second(doc); !st) {
                return makeError("save migration {} -> {} failed: {}", doc.version, doc.version + 1, st.error().message);
            }
        }
        doc.version++;
    }
    return {};
}

// ---- disk -------------------------------------------------------------------------------------------------------

std::mutex& SaveGameSystem::slotMutex(const std::string& slot) {
    std::lock_guard lock(m_slotMutexesGuard);
    auto& m = m_slotMutexes[slot];
    if (!m) m = std::make_unique<std::mutex>();
    return *m;
}

Status SaveGameSystem::writeDocument(std::string_view slot, const serial::Document& doc, u64* bytesOut) {
    OX_PROFILE_ZONE();
    if (!isValidSlotName(slot)) return makeError("invalid save slot name '{}'", slot);
    if (m_dir.empty()) return makeError("save directory unavailable");
    const std::vector<std::byte> encoded = serial::encodeBinary(doc);
    std::lock_guard slotLock(slotMutex(std::string(slot)));

    const std::filesystem::path path = slotPath(slot);
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    std::filesystem::path bak = path;
    bak += ".bak";
    std::error_code ec;
    std::filesystem::create_directories(m_dir, ec);

    auto fault = [&](SaveWriteStage stage, std::span<const std::byte>& bytes) {
        return m_faultHook && !m_faultHook(stage, bytes);
    };

    std::span<const std::byte> bytes(encoded);
    const bool failWrite = fault(SaveWriteStage::WriteTemp, bytes);
    if (auto st = writeAndSync(tmp, bytes, !fault(SaveWriteStage::Fsync, bytes)); !st || failWrite) {
        std::filesystem::remove(tmp, ec);
        return st ? makeError("save '{}': write interrupted", slot) : st;
    }
    const bool hadPrevious = std::filesystem::exists(path, ec);
    if (hadPrevious) {
        if (fault(SaveWriteStage::BackupRename, bytes)) {
            std::filesystem::remove(tmp, ec);
            return makeError("save '{}': backup failed", slot);
        }
        std::filesystem::rename(path, bak, ec);
        if (ec) {
            std::filesystem::remove(tmp, ec);
            return makeError("save '{}': cannot back up previous save: {}", slot, ec.message());
        }
    }
    if (fault(SaveWriteStage::CommitRename, bytes)) {
        ec = std::make_error_code(std::errc::io_error);
    } else {
        std::filesystem::rename(tmp, path, ec);
    }
    if (ec) {
        const std::string msg = ec.message();
        std::filesystem::remove(tmp, ec);
        if (hadPrevious) std::filesystem::rename(bak, path, ec); // roll back
        return makeError("save '{}': cannot commit: {}", slot, msg);
    }
    syncDirectory(m_dir);
    if (bytesOut) *bytesOut = encoded.size();
    return {};
}

Result<serial::Document> SaveGameSystem::readDocument(std::string_view slot, bool* fromBackup,
                                                      u32* migratedFrom) const {
    OX_PROFILE_ZONE();
    if (!isValidSlotName(slot)) return makeError("invalid save slot name '{}'", slot);
    const std::filesystem::path path = slotPath(slot);
    std::filesystem::path bak = path;
    bak += ".bak";
    if (fromBackup) *fromBackup = false;
    auto doc = decodeSave(path);
    if (!doc) {
        std::error_code ec;
        if (!std::filesystem::exists(bak, ec)) return doc.error();
        OX_LOG_WARN("save", "slot '{}' is unreadable ({}); using backup", slot, doc.error().message);
        auto backup = decodeSave(bak);
        if (!backup) return makeError("slot '{}': save and backup unreadable: {}; {}", slot, doc.error().message,
                                      backup.error().message);
        doc = std::move(backup);
        if (fromBackup) *fromBackup = true;
    }
    if (migratedFrom) *migratedFrom = doc->version;
    if (auto st = migrate(*doc); !st) return st.error();
    return doc;
}

// ---- sync API ---------------------------------------------------------------------------------------------------

Result<SaveResult> SaveGameSystem::save(std::string_view slot, const World& world, SaveKind kind,
                                        std::string displayName) {
    OX_PROFILE_ZONE();
    if (!isValidSlotName(slot)) return makeError("invalid save slot name '{}'", slot);
    serial::Document doc = snapshot(slot, world, kind, std::move(displayName));
    SaveResult r{std::string(slot), slotPath(slot), 0};
    if (auto st = writeDocument(slot, doc, &r.bytes); !st) {
        failed.emit(st.error().message);
        return st.error();
    }
    saved.emit(r);
    return r;
}

Result<LoadResult> SaveGameSystem::load(std::string_view slot, World& world) {
    bool fromBackup = false;
    u32 migratedFrom = 0;
    auto doc = readDocument(slot, &fromBackup, &migratedFrom);
    if (!doc) {
        failed.emit(doc.error().message);
        return doc.error();
    }
    auto r = apply(*doc, world);
    if (!r) return r;
    r->fromBackup = fromBackup;
    r->migratedFrom = migratedFrom;
    loaded.emit(*r);
    return r;
}

Result<LoadResult> SaveGameSystem::finishLoad(Result<serial::Document> doc, bool fromBackup, u32 migratedFrom) {
    if (!doc) {
        failed.emit(doc.error().message);
        return doc.error();
    }
    const SaveGameHeader header = readHeader(*doc);
    World* world = nullptr;
    if (m_levelLoader && !header.level.empty()) world = m_levelLoader(header.level);
    if (!world && m_worldProvider) world = m_worldProvider();
    if (!world) {
        auto err = makeError("cannot load save '{}': no world for level '{}'", header.slot, header.level);
        failed.emit(err.message);
        return err;
    }
    auto r = apply(*doc, *world);
    if (!r) {
        failed.emit(r.error().message);
        return r;
    }
    r->fromBackup = fromBackup;
    r->migratedFrom = migratedFrom;
    loaded.emit(*r);
    return r;
}

Result<LoadResult> SaveGameSystem::load(std::string_view slot) {
    bool fromBackup = false;
    u32 migratedFrom = 0;
    auto doc = readDocument(slot, &fromBackup, &migratedFrom);
    return finishLoad(std::move(doc), fromBackup, migratedFrom);
}

// ---- async API --------------------------------------------------------------------------------------------------

void SaveGameSystem::runJob(std::function<void()> fn) {
    if (!m_jobs) {
        fn();
        return;
    }
    JobHandle h = m_jobs->submit(std::move(fn));
    std::lock_guard lock(m_opsMutex);
    std::erase_if(m_jobHandles, [](const JobHandle& j) { return j.done(); });
    m_jobHandles.push_back(std::move(h));
}

SaveHandle SaveGameSystem::saveAsync(std::string_view slot, const World& world, SaveKind kind,
                                     std::string displayName) {
    OX_PROFILE_ZONE();
    auto handle = std::make_shared<SaveOperation<SaveResult>>();
    if (!isValidSlotName(slot)) {
        handle->finish(makeError("invalid save slot name '{}'", slot));
        m_pendingSaves.push_back({handle});
        return handle;
    }
    // The snapshot is a deep copy (value tree) taken now; the world is free to change afterwards.
    auto doc = std::make_shared<serial::Document>(snapshot(slot, world, kind, std::move(displayName)));
    runJob([this, handle, doc, slotName = std::string(slot)] {
        OX_PROFILE_ZONE_N("SaveGameWrite");
        SaveResult r{slotName, slotPath(slotName), 0};
        auto st = writeDocument(slotName, *doc, &r.bytes);
        if (st) {
            handle->finish(std::move(r));
        } else {
            handle->finish(st.error());
        }
    });
    m_pendingSaves.push_back({handle});
    return handle;
}

LoadHandle SaveGameSystem::loadAsync(std::string_view slot) {
    auto handle = std::make_shared<SaveOperation<LoadResult>>();
    auto state = std::make_shared<LoadState>();
    runJob([this, state, slotName = std::string(slot)] {
        OX_PROFILE_ZONE_N("SaveGameRead");
        bool fb = false;
        u32 mf = 0;
        auto doc = readDocument(slotName, &fb, &mf);
        state->doc.emplace(std::move(doc));
        state->fromBackup = fb;
        state->migratedFrom = mf;
        state->ready.store(true, std::memory_order_release);
    });
    m_pendingLoads.push_back({handle, state});
    return handle;
}

void SaveGameSystem::update(f64 realDt, bool playing) {
    OX_PROFILE_ZONE();
    for (auto it = m_pendingLoads.begin(); it != m_pendingLoads.end();) {
        if (!it->state->ready.load(std::memory_order_acquire)) {
            ++it;
            continue;
        }
        PendingLoad p = std::move(*it);
        it = m_pendingLoads.erase(it);
        p.handle->finish(finishLoad(std::move(*p.state->doc), p.state->fromBackup, p.state->migratedFrom));
    }
    for (auto it = m_pendingSaves.begin(); it != m_pendingSaves.end();) {
        if (!it->handle->done()) {
            ++it;
            continue;
        }
        const auto& r = it->handle->result();
        if (r) {
            saved.emit(*r);
        } else {
            failed.emit(r.error().message);
        }
        it = m_pendingSaves.erase(it);
    }
    if (!playing) return;
    m_playTime += realDt;
    if (m_config.autosaveInterval > 0.0) {
        m_sinceAutosave += realDt;
        if (m_sinceAutosave >= m_config.autosaveInterval) {
            m_sinceAutosave = 0.0;
            autosave();
        }
    }
}

void SaveGameSystem::waitIdle() {
    if (m_jobs) {
        std::vector<JobHandle> handles;
        {
            std::lock_guard lock(m_opsMutex);
            handles.swap(m_jobHandles);
        }
        for (const auto& h : handles) m_jobs->wait(h);
    }
    update(0.0, false);
}

usize SaveGameSystem::pendingOperations() const { return m_pendingLoads.size() + m_pendingSaves.size(); }

// ---- convenience ------------------------------------------------------------------------------------------------

Result<SaveResult> SaveGameSystem::quickSave() {
    World* w = m_worldProvider ? m_worldProvider() : nullptr;
    if (!w) return makeError("quick save: no world");
    return save(m_config.quickSlot, *w, SaveKind::Quick, "Quick Save");
}

Result<LoadResult> SaveGameSystem::quickLoad() { return load(m_config.quickSlot); }

std::vector<std::string> SaveGameSystem::autosaveSlots() const {
    std::vector<std::string> out;
    for (u32 i = 0; i < std::max(m_config.maxAutosaves, 1u); ++i) out.push_back("autosave" + std::to_string(i));
    return out;
}

std::string SaveGameSystem::nextAutosaveSlot() {
    const auto slots = autosaveSlots();
    if (!m_autosaveCursorValid) {
        // Continue after the newest existing autosave (survives restarts).
        std::error_code ec;
        std::optional<std::filesystem::file_time_type> newest;
        u32 newestIndex = 0;
        bool any = false;
        for (u32 i = 0; i < slots.size(); ++i) {
            const auto t = std::filesystem::last_write_time(slotPath(slots[i]), ec);
            if (ec) continue;
            any = true;
            if (!newest || t > *newest) {
                newest = t;
                newestIndex = i;
            }
        }
        m_autosaveCursor = any ? (newestIndex + 1) % u32(slots.size()) : 0;
        m_autosaveCursorValid = true;
    }
    const std::string slot = slots[m_autosaveCursor];
    m_autosaveCursor = (m_autosaveCursor + 1) % u32(slots.size());
    return slot;
}

SaveHandle SaveGameSystem::autosave() {
    World* w = m_worldProvider ? m_worldProvider() : nullptr;
    if (!w) {
        auto h = std::make_shared<SaveOperation<SaveResult>>();
        h->finish(makeError("autosave: no world"));
        return h;
    }
    return saveAsync(nextAutosaveSlot(), *w, SaveKind::Auto, "Autosave");
}

void SaveGameSystem::onLevelChanged(const std::string& newLevel) {
    if (m_config.autosaveOnLevelChange && !m_level.empty() && m_worldProvider && m_worldProvider()) autosave();
    m_level = newLevel;
    m_sinceAutosave = 0.0;
}

// ---- slots ------------------------------------------------------------------------------------------------------

Result<SaveSlotInfo> SaveGameSystem::slotInfo(std::string_view slot) const {
    if (!isValidSlotName(slot)) return makeError("invalid save slot name '{}'", slot);
    SaveSlotInfo info;
    info.path = slotPath(slot);
    std::filesystem::path bak = info.path;
    bak += ".bak";
    std::error_code ec;
    info.hasBackup = std::filesystem::exists(bak, ec);
    const bool hasMain = std::filesystem::exists(info.path, ec);
    if (!hasMain && !info.hasBackup) return makeError("no save in slot '{}'", slot);
    auto doc = hasMain ? decodeSave(info.path) : Result<serial::Document>(makeError("missing"));
    if (!doc) {
        info.corrupted = true;
        if (info.hasBackup) doc = decodeSave(bak);
    }
    if (doc) {
        info.header = readHeader(*doc);
        const Value* thumb = doc->root.find("thumbnail");
        info.hasThumbnail = thumb && thumb->size() > 0;
    }
    info.header.slot = std::string(slot);
    info.fileSize = hasMain ? std::filesystem::file_size(info.path, ec) : 0;
    return info;
}

std::vector<SaveSlotInfo> SaveGameSystem::listSlots() const {
    std::vector<SaveSlotInfo> out;
    std::error_code ec;
    if (m_dir.empty() || !std::filesystem::is_directory(m_dir, ec)) return out;
    std::set<std::string> slots;
    for (const auto& entry : std::filesystem::directory_iterator(m_dir, ec)) {
        if (!entry.is_regular_file()) continue;
        std::string name = entry.path().filename().string();
        for (const std::string& suffix : {m_config.extension, m_config.extension + ".bak"}) {
            if (name.size() > suffix.size() && name.ends_with(suffix)) {
                const std::string slot = name.substr(0, name.size() - suffix.size());
                if (isValidSlotName(slot)) slots.insert(slot);
            }
        }
    }
    for (const auto& s : slots) {
        if (auto info = slotInfo(s)) out.push_back(std::move(*info));
    }
    std::stable_sort(out.begin(), out.end(), [](const SaveSlotInfo& a, const SaveSlotInfo& b) {
        return a.header.timestamp > b.header.timestamp;
    });
    return out;
}

Result<std::vector<std::byte>> SaveGameSystem::thumbnail(std::string_view slot) const {
    auto doc = readDocument(slot);
    if (!doc) return doc.error();
    const Value* thumb = doc->root.find("thumbnail");
    if (!thumb || !thumb->isPacked()) return std::vector<std::byte>{};
    auto bytes = thumb->packedBytes();
    return std::vector<std::byte>(bytes.begin(), bytes.end());
}

bool SaveGameSystem::exists(std::string_view slot) const {
    std::error_code ec;
    return isValidSlotName(slot) && std::filesystem::exists(slotPath(slot), ec);
}

bool SaveGameSystem::deleteSlot(std::string_view slot) {
    if (!isValidSlotName(slot)) return false;
    std::lock_guard lock(slotMutex(std::string(slot)));
    std::error_code ec;
    const auto path = slotPath(slot);
    const bool removed = std::filesystem::remove(path, ec);
    for (const char* suffix : {".bak", ".tmp"}) {
        auto p = path;
        p += suffix;
        std::filesystem::remove(p, ec);
    }
    return removed;
}

Result<std::string> SaveGameSystem::exportJson(std::string_view slot, const std::filesystem::path& outPath) const {
    if (!isValidSlotName(slot)) return makeError("invalid save slot name '{}'", slot);
    auto bytes = serial::readFileBytes(slotPath(slot));
    if (!bytes) return bytes.error();
    auto json = serial::binaryToJson(*bytes, 2);
    if (!json) return json.error();
    if (!outPath.empty()) {
        const std::string text = *json + "\n";
        if (auto st = serial::writeFileAtomic(outPath, std::as_bytes(std::span(text.data(), text.size()))); !st) {
            return st.error();
        }
    }
    return json;
}

} // namespace ox
