#include <oxwald/assets/asset_registry.hpp>
#include <oxwald/core/file_watcher.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/core/profile.hpp>

#include "internal.hpp"

#include <fstream>
#include <map>
#include <queue>

namespace fs = std::filesystem;

namespace ox::assets {

namespace {

constexpr u32 kRecordVersion = 1;

struct FileStamp {
    u64 size = 0;
    i64 mtime = 0;
    bool exists = false;
    bool operator==(const FileStamp&) const = default;
};

FileStamp stampOf(const fs::path& p) {
    std::error_code ec;
    FileStamp s;
    const auto size = fs::file_size(p, ec);
    if (ec) return s;
    const auto time = fs::last_write_time(p, ec);
    if (ec) return s;
    s.exists = true;
    s.size = size;
    s.mtime = static_cast<i64>(time.time_since_epoch().count());
    return s;
}

u64 hashFile(const fs::path& p) {
    auto bytes = detail::readFile(p);
    return bytes ? fnv1a64(std::span<const std::byte>(*bytes)) : 0;
}

u64 settingsHash(const nlohmann::ordered_json& settings) { return fnv1a64(settings.dump()); }

bool isHiddenComponent(const fs::path& rel) {
    for (const auto& part : rel) {
        const std::string s = part.string();
        if (!s.empty() && s[0] == '.') return true;
    }
    return false;
}

} // namespace

struct AssetRegistry::ImportRecord {
    std::string source;
    u64 sourceHash = 0;
    FileStamp sourceStamp;
    u64 settingsHash = 0;
    std::string importer;
    u32 importerVersion = 0;
    struct SourceDep {
        std::string path; // absolute, generic
        FileStamp stamp;
    };
    std::vector<SourceDep> sourceDeps;
    struct Artifact {
        Uuid uuid;
        AssetType type = AssetType::Unknown;
        std::string name;
        std::string file; // relative to cache dir
        std::vector<Uuid> dependencies;
        nlohmann::ordered_json info;
    };
    std::vector<Artifact> artifacts;

    nlohmann::ordered_json toJson() const {
        nlohmann::ordered_json j;
        j["version"] = kRecordVersion;
        j["source"] = source;
        j["sourceHash"] = sourceHash;
        j["sourceSize"] = sourceStamp.size;
        j["sourceMtime"] = sourceStamp.mtime;
        j["settingsHash"] = settingsHash;
        j["importer"] = importer;
        j["importerVersion"] = importerVersion;
        auto& deps = j["sourceDeps"] = nlohmann::ordered_json::array();
        for (const auto& d : sourceDeps) deps.push_back({{"path", d.path}, {"size", d.stamp.size}, {"mtime", d.stamp.mtime}});
        auto& arts = j["artifacts"] = nlohmann::ordered_json::array();
        for (const auto& a : artifacts) {
            nlohmann::ordered_json aj;
            aj["uuid"] = a.uuid.toString();
            aj["type"] = std::string(assetTypeName(a.type));
            aj["typeId"] = u32(a.type);
            aj["name"] = a.name;
            aj["file"] = a.file;
            auto& dj = aj["dependencies"] = nlohmann::ordered_json::array();
            for (const auto& d : a.dependencies) dj.push_back(d.toString());
            aj["info"] = a.info;
            arts.push_back(std::move(aj));
        }
        return j;
    }
    static std::optional<ImportRecord> fromJson(const nlohmann::ordered_json& j) {
        if (!j.is_object() || j.value("version", 0u) != kRecordVersion) return std::nullopt;
        ImportRecord r;
        r.source = j.value("source", std::string{});
        r.sourceHash = j.value("sourceHash", u64{0});
        r.sourceStamp = {j.value("sourceSize", u64{0}), j.value("sourceMtime", i64{0}), true};
        r.settingsHash = j.value("settingsHash", u64{0});
        r.importer = j.value("importer", std::string{});
        r.importerVersion = j.value("importerVersion", 0u);
        for (const auto& d : j.value("sourceDeps", nlohmann::ordered_json::array())) {
            r.sourceDeps.push_back({d.value("path", std::string{}), {d.value("size", u64{0}), d.value("mtime", i64{0}), true}});
        }
        for (const auto& a : j.value("artifacts", nlohmann::ordered_json::array())) {
            Artifact art;
            art.uuid = Uuid::parse(a.value("uuid", std::string{})).value_or(Uuid{});
            art.type = AssetType(a.value("typeId", 0u));
            art.name = a.value("name", std::string{});
            art.file = a.value("file", std::string{});
            for (const auto& d : a.value("dependencies", nlohmann::ordered_json::array())) {
                if (auto u = Uuid::parse(d.get<std::string>())) art.dependencies.push_back(*u);
            }
            art.info = a.value("info", nlohmann::ordered_json::object());
            if (art.uuid.isValid()) r.artifacts.push_back(std::move(art));
        }
        return r;
    }
};

struct AssetRegistry::Entry {
    AssetInfo info;
    // Main assets only:
    AssetMeta meta;
    std::optional<ImportRecord> record;
    bool failed = false; // last import failed (do not retry on every access until the source changes)
    FileStamp failedStamp;
};

AssetRegistry::AssetRegistry(fs::path projectDir) : AssetRegistry(std::move(projectDir), Options{}) {}

AssetRegistry::AssetRegistry(fs::path projectDir, Options options)
    : m_projectDir(fs::absolute(projectDir).lexically_normal()), m_options(options) {
    m_assetsDir = m_projectDir / "Assets";
    m_cacheDir = m_projectDir / ".oxcache";
    std::error_code ec;
    fs::create_directories(m_assetsDir, ec);
    fs::create_directories(m_cacheDir / "artifacts", ec);
    fs::create_directories(m_cacheDir / "imports", ec);
    m_importers.addBuiltins();
}

AssetRegistry::~AssetRegistry() { stopWatching(); }

std::string AssetRegistry::relativePath(const fs::path& abs) const {
    const auto rel = fs::absolute(abs).lexically_normal().lexically_relative(m_assetsDir);
    if (rel.empty() || *rel.begin() == "..") return {};
    return rel.generic_string();
}

AssetRegistry::Entry* AssetRegistry::findEntry(const Uuid& uuid) {
    auto it = m_entries.find(uuid);
    return it == m_entries.end() ? nullptr : it->second.get();
}

void AssetRegistry::loadRecordLocked(Entry& entry) {
    // Drop previous sub-assets.
    for (const Uuid& sub : entry.info.subAssets) {
        if (Entry* s = findEntry(sub)) m_pathToUuid.erase(s->info.path);
        m_entries.erase(sub);
    }
    entry.info.subAssets.clear();
    entry.info.imported = false;
    entry.info.artifact.clear();
    entry.info.dependencies.clear();
    entry.record.reset();
    auto bytes = detail::readFile(m_cacheDir / "imports" / (entry.info.uuid.toString() + ".json"));
    if (!bytes) return;
    auto j = nlohmann::ordered_json::parse(detail::asString(*bytes), nullptr, false);
    if (j.is_discarded()) return;
    entry.record = ImportRecord::fromJson(j);
    if (!entry.record) return;
    for (const auto& a : entry.record->artifacts) {
        if (a.name.empty()) {
            entry.info.type = a.type;
            entry.info.artifact = m_cacheDir / a.file;
            entry.info.dependencies = a.dependencies;
            entry.info.info = a.info;
            entry.info.imported = fs::exists(entry.info.artifact);
            continue;
        }
        auto sub = std::make_unique<Entry>();
        sub->info.uuid = a.uuid;
        sub->info.type = a.type;
        sub->info.path = entry.info.path + "#" + a.name;
        sub->info.parent = entry.info.uuid;
        sub->info.subName = a.name;
        sub->info.dependencies = a.dependencies;
        sub->info.artifact = m_cacheDir / a.file;
        sub->info.imported = fs::exists(sub->info.artifact);
        sub->info.info = a.info;
        entry.info.subAssets.push_back(a.uuid);
        m_pathToUuid[sub->info.path] = a.uuid;
        m_entries[a.uuid] = std::move(sub);
    }
}

void AssetRegistry::rebuildDependentsLocked() {
    m_dependents.clear();
    m_sourceDepToAssets.clear();
    for (const auto& [uuid, e] : m_entries) {
        for (const Uuid& d : e->info.dependencies) m_dependents[d].insert(uuid);
        if (e->record) {
            for (const auto& sd : e->record->sourceDeps) m_sourceDepToAssets[sd.path].insert(uuid);
        }
    }
}

AssetRegistry::Entry* AssetRegistry::addSourceLocked(const fs::path& absPath, bool* createdMeta) {
    if (createdMeta) *createdMeta = false;
    const std::string rel = relativePath(absPath);
    if (rel.empty()) return nullptr;
    if (auto it = m_pathToUuid.find(rel); it != m_pathToUuid.end()) return findEntry(it->second);
    IAssetImporter* importer = m_importers.findForPath(absPath);
    if (!importer) return nullptr;
    const fs::path metaPath = metaPathFor(absPath);
    AssetMeta meta;
    bool write = false;
    if (auto existing = readMeta(metaPath)) {
        meta = std::move(*existing);
        if (m_entries.count(meta.uuid)) { // copied file + meta: new identity
            meta.uuid = Uuid::generate();
            write = true;
        }
    } else {
        meta.uuid = Uuid::generate();
        meta.importer = std::string(importer->name());
        meta.importerVersion = importer->version();
        meta.settings = importer->defaultSettingsFor(absPath);
        write = true;
        if (createdMeta) *createdMeta = true;
    }
    if (meta.importer.empty()) {
        meta.importer = std::string(importer->name());
        write = true;
    }
    if (write && !writeMeta(metaPath, meta)) OX_LOG_ERROR("assets", "cannot write {}", metaPath.string());
    auto entry = std::make_unique<Entry>();
    entry->info.uuid = meta.uuid;
    entry->info.type = importer->mainType();
    entry->info.path = rel;
    entry->info.importer = meta.importer;
    entry->meta = std::move(meta);
    Entry* raw = entry.get();
    m_pathToUuid[rel] = raw->info.uuid;
    m_entries[raw->info.uuid] = std::move(entry);
    loadRecordLocked(*raw);
    if (raw->record && raw->record->source != rel) raw->record.reset(); // stale record of a different file
    return raw;
}

ScanResult AssetRegistry::scan() {
    OX_PROFILE_ZONE();
    std::vector<std::tuple<Uuid, std::string, std::string>> moved;
    std::vector<Uuid> removed;
    ScanResult result;
    {
        std::lock_guard lock(m_mutex);
        struct Found {
            fs::path abs;
            std::string rel;
            std::optional<AssetMeta> meta;
        };
        std::vector<Found> sources;
        std::vector<fs::path> metas;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(m_assetsDir, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            const fs::path rel = it->path().lexically_relative(m_assetsDir);
            if (isHiddenComponent(rel)) {
                if (it->is_directory()) it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file()) continue;
            if (it->path().extension() == kMetaExtension) {
                metas.push_back(it->path());
            } else if (m_importers.findForPath(it->path())) {
                sources.push_back({it->path(), rel.generic_string(), std::nullopt});
            }
        }
        std::sort(sources.begin(), sources.end(), [](const Found& a, const Found& b) { return a.rel < b.rel; });
        result.found = u32(sources.size());

        std::set<fs::path> sourceSet;
        for (const auto& s : sources) sourceSet.insert(s.abs);
        // Orphan metas (source vanished) are candidates for move detection.
        struct Orphan {
            fs::path metaPath;
            AssetMeta meta;
            u64 sourceHash = 0;
            bool used = false;
        };
        std::vector<Orphan> orphans;
        for (const auto& m : metas) {
            fs::path src = m;
            src.replace_extension();
            if (sourceSet.count(src)) continue;
            if (auto meta = readMeta(m)) {
                Orphan o{m, *meta, 0, false};
                auto rec = detail::readFile(m_cacheDir / "imports" / (meta->uuid.toString() + ".json"));
                if (rec) {
                    auto j = nlohmann::ordered_json::parse(detail::asString(*rec), nullptr, false);
                    if (!j.is_discarded()) o.sourceHash = j.value("sourceHash", u64{0});
                }
                orphans.push_back(std::move(o));
            }
        }

        // Pass 1: read metas; detect duplicates (copied file + meta).
        std::unordered_map<Uuid, usize> owner;
        for (usize i = 0; i < sources.size(); ++i) {
            auto meta = readMeta(metaPathFor(sources[i].abs));
            if (!meta) continue;
            sources[i].meta = std::move(*meta);
        }
        // Keep the UUID for the file whose path matches the known/record path, re-identify the others.
        for (usize i = 0; i < sources.size(); ++i) {
            if (!sources[i].meta) continue;
            const Uuid id = sources[i].meta->uuid;
            auto it = owner.find(id);
            if (it == owner.end()) {
                owner[id] = i;
                continue;
            }
            usize keep = it->second, other = i;
            if (Entry* e = findEntry(id); e && e->info.path == sources[i].rel) std::swap(keep, other);
            owner[id] = keep;
            sources[other].meta->uuid = Uuid::generate();
            (void)writeMeta(metaPathFor(sources[other].abs), *sources[other].meta);
            owner[sources[other].meta->uuid] = other;
            ++result.duplicatesFixed;
        }
        // Pass 2: meta-less sources: adopt an orphan meta with the same content (moved without its meta).
        for (auto& s : sources) {
            if (s.meta) continue;
            if (!orphans.empty()) {
                const u64 h = hashFile(s.abs);
                for (auto& o : orphans) {
                    if (o.used || o.sourceHash == 0 || o.sourceHash != h || owner.count(o.meta.uuid)) continue;
                    o.used = true;
                    s.meta = o.meta;
                    (void)writeMeta(metaPathFor(s.abs), *s.meta);
                    std::error_code rmEc;
                    fs::remove(o.metaPath, rmEc);
                    break;
                }
            }
            if (!s.meta) {
                IAssetImporter* importer = m_importers.findForPath(s.abs);
                AssetMeta meta;
                meta.uuid = Uuid::generate();
                meta.importer = std::string(importer->name());
                meta.importerVersion = importer->version();
                meta.settings = importer->defaultSettingsFor(s.abs);
                (void)writeMeta(metaPathFor(s.abs), meta);
                s.meta = std::move(meta);
                ++result.metasCreated;
            }
        }
        for (auto& o : orphans) {
            if (o.used || !m_options.deleteOrphanMetas) continue;
            std::error_code rmEc;
            fs::remove(o.metaPath, rmEc);
        }

        // Rebuild the main entries, keeping records; report moves and removals.
        std::unordered_map<Uuid, std::unique_ptr<Entry>> old;
        for (auto& [id, e] : m_entries) {
            if (!e->info.parent.isValid()) old[id] = std::move(e);
        }
        m_entries.clear();
        m_pathToUuid.clear();
        for (auto& s : sources) {
            auto e = std::make_unique<Entry>();
            IAssetImporter* importer = m_importers.find(s.meta->importer);
            if (!importer) importer = m_importers.findForPath(s.abs);
            e->info.uuid = s.meta->uuid;
            e->info.type = importer ? importer->mainType() : AssetType::Unknown;
            e->info.path = s.rel;
            e->info.importer = importer ? std::string(importer->name()) : s.meta->importer;
            e->meta = std::move(*s.meta);
            if (auto it = old.find(e->info.uuid); it != old.end()) {
                if (it->second->info.path != s.rel) {
                    moved.emplace_back(e->info.uuid, it->second->info.path, s.rel);
                }
                e->failed = it->second->failed;
                e->failedStamp = it->second->failedStamp;
                old.erase(it);
            }
            Entry* raw = e.get();
            m_pathToUuid[s.rel] = raw->info.uuid;
            m_entries[raw->info.uuid] = std::move(e);
        }
        for (auto& [id, e] : old) removed.push_back(id);
        std::vector<Uuid> mains;
        for (auto& [id, e] : m_entries) mains.push_back(id);
        for (const Uuid& id : mains) {
            Entry& e = *m_entries[id];
            loadRecordLocked(e);
            if (e.record && e.record->source != e.info.path) {
                // Moved: the record still describes the old path; keep artifacts, fix the path.
                e.record->source = e.info.path;
                (void)detail::writeFile(m_cacheDir / "imports" / (id.toString() + ".json"),
                                        detail::asBytes(e.record->toJson().dump(2)));
                loadRecordLocked(e); // sub-asset paths follow the new location
            }
        }
        // Clean up cache files of removed assets.
        for (const Uuid& id : removed) {
            std::error_code rmEc;
            auto recPath = m_cacheDir / "imports" / (id.toString() + ".json");
            if (auto bytes = detail::readFile(recPath)) {
                auto j = nlohmann::ordered_json::parse(detail::asString(*bytes), nullptr, false);
                if (auto rec = j.is_discarded() ? std::nullopt : ImportRecord::fromJson(j)) {
                    for (const auto& a : rec->artifacts) fs::remove(m_cacheDir / a.file, rmEc);
                }
            }
            fs::remove(recPath, rmEc);
        }
        rebuildDependentsLocked();
        result.moved = u32(moved.size());
        result.removed = u32(removed.size());
    }
    for (auto& [id, from, to] : moved) onMoved.emit(id, from, to);
    for (const Uuid& id : removed) onRemoved.emit(id);
    return result;
}

bool AssetRegistry::isStaleLocked(const Entry& e) {
    if (!e.record) return true;
    const ImportRecord& r = *e.record;
    IAssetImporter* importer = m_importers.find(e.info.importer);
    if (!importer) return false;
    if (r.importer != importer->name() || r.importerVersion != importer->version()) return true;
    if (r.settingsHash != settingsHash(e.meta.settings)) return true;
    for (const auto& a : r.artifacts) {
        if (!fs::exists(m_cacheDir / a.file)) return true;
    }
    const fs::path abs = m_assetsDir / e.info.path;
    const FileStamp now = stampOf(abs);
    if (!(now.size == r.sourceStamp.size && now.mtime == r.sourceStamp.mtime)) {
        if (hashFile(abs) != r.sourceHash) return true;
    }
    for (const auto& d : r.sourceDeps) {
        const FileStamp s = stampOf(d.path);
        if (s.size != d.stamp.size || s.mtime != d.stamp.mtime) return true;
    }
    return false;
}

std::optional<Uuid> AssetRegistry::resolveForImport(const fs::path& file, const nlohmann::ordered_json& hint) {
    std::lock_guard lock(m_mutex);
    if (relativePath(file).empty()) return std::nullopt;
    bool created = false;
    Entry* e = addSourceLocked(fs::absolute(file).lexically_normal(), &created);
    if (!e) return std::nullopt;
    // A never-imported asset still has auto-generated settings: apply the usage hint (normal map, linear).
    if (!e->record && hint.is_object() && !hint.empty()) {
        auto merged = e->meta.settings;
        merged.merge_patch(hint);
        if (merged != e->meta.settings) {
            e->meta.settings = merged;
            (void)writeMeta(metaPathFor(m_assetsDir / e->info.path), e->meta);
        }
    }
    return e->info.uuid;
}

Status AssetRegistry::importLocked(Entry& entry, bool force, std::vector<Uuid>* changed) {
    OX_PROFILE_ZONE();
    if (entry.info.parent.isValid()) {
        Entry* parent = findEntry(entry.info.parent);
        if (!parent) return makeError("sub-asset {} has no parent", entry.info.path);
        return importLocked(*parent, force, changed);
    }
    const fs::path abs = m_assetsDir / entry.info.path;
    if (!force && !isStaleLocked(entry)) return {};
    const FileStamp stamp = stampOf(abs);
    if (!force && entry.failed && entry.failedStamp == stamp) {
        return makeError("{}: previous import failed", entry.info.path);
    }
    IAssetImporter* importer = m_importers.find(entry.info.importer);
    if (!importer) importer = m_importers.findForPath(abs);
    if (!importer) return makeError("{}: no importer", entry.info.path);

    ImportContext::Callbacks callbacks;
    callbacks.resolve = [this](const fs::path& p, const nlohmann::ordered_json& hint) { return resolveForImport(p, hint); };
    AssetMeta meta = entry.meta;
    meta.importerVersion = importer->version();
    ImportContext ctx(abs, entry.info.path, meta, callbacks);
    Status st;
    try {
        st = importer->import(ctx);
    } catch (const std::exception& e) {
        st = makeError("importer threw: {}", e.what());
    }
    if (!st) {
        entry.failed = true;
        entry.failedStamp = stamp;
        OX_LOG_ERROR("assets", "import of {} failed: {}", entry.info.path, st.error().message);
        return makeError("{}: {}", entry.info.path, st.error().message);
    }
    entry.failed = false;

    // Write artifacts, then the record (a crash in between leaves a stale record => reimport).
    ImportRecord rec;
    rec.source = entry.info.path;
    rec.sourceHash = hashFile(abs);
    rec.sourceStamp = stamp;
    rec.settingsHash = settingsHash(entry.meta.settings);
    rec.importer = std::string(importer->name());
    rec.importerVersion = importer->version();
    for (const auto& d : ctx.sourceDependencies()) {
        const std::string p = fs::absolute(d).lexically_normal().generic_string();
        if (p == fs::absolute(abs).lexically_normal().generic_string()) continue;
        rec.sourceDeps.push_back({p, stampOf(d)});
    }
    std::set<Uuid> produced;
    for (const auto& a : ctx.artifacts()) produced.insert(a.uuid);
    for (auto& a : ctx.artifacts()) {
        ImportRecord::Artifact art;
        art.uuid = a.uuid;
        art.type = a.type;
        art.name = a.name;
        art.file = "artifacts/" + a.uuid.toString() + std::string(artifactExtension(a.type));
        for (const Uuid& d : a.dependencies) {
            if (d != a.uuid && (produced.count(d) || m_entries.count(d))) art.dependencies.push_back(d);
        }
        art.info = a.info;
        if (auto ws = detail::writeFile(m_cacheDir / art.file, a.data); !ws) return ws.error();
        rec.artifacts.push_back(std::move(art));
    }
    // Remove artifacts of sub-assets that disappeared.
    if (entry.record) {
        for (const auto& old : entry.record->artifacts) {
            if (!produced.count(old.uuid)) {
                std::error_code ec;
                fs::remove(m_cacheDir / old.file, ec);
            }
        }
    }
    {
        const auto recPath = m_cacheDir / "imports" / (entry.info.uuid.toString() + ".json");
        if (auto ws = detail::writeFile(recPath, detail::asBytes(rec.toJson().dump(2))); !ws) return ws.error();
    }
    loadRecordLocked(entry);
    rebuildDependentsLocked();
    if (changed) {
        changed->push_back(entry.info.uuid);
        for (const Uuid& s : entry.info.subAssets) changed->push_back(s);
    }
    OX_LOG_INFO("assets", "imported {} ({} artifacts)", entry.info.path, rec.artifacts.size());
    return {};
}

ImportStats AssetRegistry::importAll(bool force) {
    OX_PROFILE_ZONE();
    std::lock_guard lock(m_mutex);
    ImportStats stats;
    std::set<Uuid> done;
    // Models first: they hint texture settings (normal/linear) of textures that were never imported.
    for (int pass = 0; pass < 4; ++pass) {
        std::vector<Uuid> todo;
        for (const auto& [id, e] : m_entries) {
            if (!e->info.parent.isValid() && !done.count(id)) todo.push_back(id);
        }
        if (todo.empty()) break;
        std::stable_sort(todo.begin(), todo.end(), [&](const Uuid& a, const Uuid& b) {
            const bool ma = m_entries[a]->info.importer == "model";
            const bool mb = m_entries[b]->info.importer == "model";
            if (ma != mb) return ma;
            return m_entries[a]->info.path < m_entries[b]->info.path;
        });
        for (const Uuid& id : todo) {
            done.insert(id);
            Entry* e = findEntry(id);
            if (!e) continue;
            if (!force && !isStaleLocked(*e)) {
                ++stats.upToDate;
                continue;
            }
            if (importLocked(*e, force, nullptr)) ++stats.imported;
            else ++stats.failed;
        }
    }
    return stats;
}

Status AssetRegistry::import(const Uuid& uuid, bool force) {
    std::vector<Uuid> changed;
    Status st;
    {
        std::lock_guard lock(m_mutex);
        Entry* e = findEntry(uuid);
        if (!e) return makeError("unknown asset {}", uuid.toString());
        st = importLocked(*e, force, &changed);
    }
    for (const Uuid& c : changed) {
        onReimported.emit(c);
        m_changed.emit(c);
    }
    return st;
}

bool AssetRegistry::needsImport(const Uuid& uuid) {
    std::lock_guard lock(m_mutex);
    Entry* e = findEntry(uuid);
    if (!e) return false;
    if (e->info.parent.isValid()) e = findEntry(e->info.parent);
    return e && isStaleLocked(*e);
}

Status AssetRegistry::setSettings(const Uuid& uuid, const nlohmann::ordered_json& settings) {
    {
        std::lock_guard lock(m_mutex);
        Entry* e = findEntry(uuid);
        if (!e || e->info.parent.isValid()) return makeError("unknown main asset {}", uuid.toString());
        e->meta.settings.merge_patch(settings);
        if (auto st = writeMeta(metaPathFor(m_assetsDir / e->info.path), e->meta); !st) return st;
    }
    return import(uuid, false);
}

std::optional<AssetInfo> AssetRegistry::info(const Uuid& uuid) {
    std::lock_guard lock(m_mutex);
    Entry* e = findEntry(uuid);
    return e ? std::optional<AssetInfo>(e->info) : std::nullopt;
}

std::optional<AssetMeta> AssetRegistry::meta(const Uuid& uuid) {
    std::lock_guard lock(m_mutex);
    Entry* e = findEntry(uuid);
    if (!e || e->info.parent.isValid()) return std::nullopt;
    return e->meta;
}

fs::path AssetRegistry::absolutePath(const Uuid& uuid) {
    std::lock_guard lock(m_mutex);
    Entry* e = findEntry(uuid);
    if (!e) return {};
    if (e->info.parent.isValid()) e = findEntry(e->info.parent);
    return e ? m_assetsDir / e->info.path : fs::path{};
}

std::vector<Uuid> AssetRegistry::dependencies(const Uuid& uuid) {
    std::lock_guard lock(m_mutex);
    Entry* e = findEntry(uuid);
    return e ? e->info.dependencies : std::vector<Uuid>{};
}

std::vector<Uuid> AssetRegistry::dependents(const Uuid& uuid) {
    std::lock_guard lock(m_mutex);
    auto it = m_dependents.find(uuid);
    return it == m_dependents.end() ? std::vector<Uuid>{} : std::vector<Uuid>(it->second.begin(), it->second.end());
}

std::vector<Uuid> AssetRegistry::collectDependencies(std::span<const Uuid> roots) {
    std::vector<Uuid> out;
    std::set<Uuid> seen;
    std::queue<Uuid> q;
    for (const Uuid& r : roots) q.push(r);
    while (!q.empty()) {
        const Uuid id = q.front();
        q.pop();
        if (!seen.insert(id).second) continue;
        auto rec = record(id); // imports on demand so dependencies are known
        if (!rec) continue;
        out.push_back(id);
        for (const Uuid& d : rec->dependencies) q.push(d);
    }
    return out;
}

std::optional<AssetRecord> AssetRegistry::record(const Uuid& uuid) {
    std::vector<Uuid> changed;
    std::optional<AssetRecord> out;
    {
        std::lock_guard lock(m_mutex);
        Entry* e = findEntry(uuid);
        if (!e) return std::nullopt;
        if (m_options.importOnDemand) {
            Entry* main = e->info.parent.isValid() ? findEntry(e->info.parent) : e;
            if (main && isStaleLocked(*main)) {
                if (!importLocked(*main, false, &changed)) return std::nullopt;
            }
            e = findEntry(uuid); // sub-asset entries are recreated by an import
            if (!e) return std::nullopt;
        }
        if (!e->info.imported) return std::nullopt;
        std::error_code ec;
        out = AssetRecord{e->info.uuid, e->info.type, e->info.path, e->info.dependencies,
                          u64(fs::file_size(e->info.artifact, ec))};
    }
    // An on-demand import is not a "change" of already loaded data (nothing could be loaded before).
    return out;
}

std::optional<Uuid> AssetRegistry::uuidForPath(std::string_view path) {
    std::lock_guard lock(m_mutex);
    std::string p(path);
    std::replace(p.begin(), p.end(), '\\', '/');
    if (p.starts_with("Assets/")) p = p.substr(7);
    auto it = m_pathToUuid.find(p);
    if (it != m_pathToUuid.end()) return it->second;
    if (auto u = Uuid::parse(p); u && m_entries.count(*u)) return *u;
    // "model.glb#Mesh/0" before the model was imported: import it (sub-assets are known afterwards).
    if (const auto hash = p.find('#'); hash != std::string::npos && m_options.importOnDemand) {
        auto main = m_pathToUuid.find(p.substr(0, hash));
        if (main != m_pathToUuid.end()) {
            if (Entry* e = findEntry(main->second); e && isStaleLocked(*e) && importLocked(*e, false, nullptr)) {
                if (auto sub = m_pathToUuid.find(p); sub != m_pathToUuid.end()) return sub->second;
            }
        }
    }
    return std::nullopt;
}

Result<std::vector<std::byte>> AssetRegistry::readArtifact(const Uuid& uuid) {
    auto rec = record(uuid);
    if (!rec) return makeError("asset {} is not available", uuid.toString());
    fs::path file;
    {
        std::lock_guard lock(m_mutex);
        Entry* e = findEntry(uuid);
        if (!e) return makeError("asset {} vanished", uuid.toString());
        file = e->info.artifact;
    }
    return detail::readFile(file);
}

Result<std::vector<std::byte>> AssetRegistry::readArtifactRange(const Uuid& uuid, u64 offset, u64 size) {
    auto rec = record(uuid);
    if (!rec) return makeError("asset {} is not available", uuid.toString());
    fs::path file;
    {
        std::lock_guard lock(m_mutex);
        file = findEntry(uuid)->info.artifact;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) return makeError("cannot open {}", file.string());
    in.seekg(std::streamoff(offset));
    std::vector<std::byte> out(size);
    in.read(reinterpret_cast<char*>(out.data()), std::streamsize(size));
    out.resize(usize(in.gcount()));
    return out;
}

std::vector<Uuid> AssetRegistry::allAssets() {
    std::lock_guard lock(m_mutex);
    std::vector<Uuid> out;
    for (const auto& [id, e] : m_entries) out.push_back(id);
    std::sort(out.begin(), out.end());
    return out;
}

Connection AssetRegistry::subscribeChanges(std::function<void(const Uuid&)> fn) { return m_changed.connect(std::move(fn)); }

void AssetRegistry::startWatching() {
    std::lock_guard lock(m_mutex);
    if (m_watcher) return;
    m_watcher = std::make_unique<FileWatcher>();
    m_watcher->setDebounce(m_options.watchDebounce);
    m_watcher->watchDirectory(m_assetsDir, [this](const FileChange& c) {
        std::lock_guard l(m_changeMutex);
        m_pendingChanges.push_back(c.path);
    });
}

void AssetRegistry::stopWatching() {
    std::unique_ptr<FileWatcher> w;
    {
        std::lock_guard lock(m_mutex);
        w = std::move(m_watcher);
    }
}

void AssetRegistry::handleChangeLocked(const fs::path& path, std::vector<Uuid>& changed) {
    fs::path source = path;
    const bool isMeta = path.extension() == kMetaExtension;
    if (isMeta) source.replace_extension();
    const std::string rel = relativePath(source);
    // Assets importing this file indirectly (.mtl, .bin, cubemap faces).
    const std::string generic = fs::absolute(source).lexically_normal().generic_string();
    if (auto it = m_sourceDepToAssets.find(generic); it != m_sourceDepToAssets.end()) {
        for (const Uuid& id : std::vector<Uuid>(it->second.begin(), it->second.end())) {
            if (Entry* e = findEntry(id); e && !e->info.parent.isValid()) (void)importLocked(*e, false, &changed); // failures are logged
        }
    }
    auto it = m_pathToUuid.find(rel);
    if (it == m_pathToUuid.end()) return;
    Entry* e = findEntry(it->second);
    if (!e) return;
    if (isMeta) {
        auto meta = readMeta(path);
        if (!meta || meta->uuid != e->meta.uuid) return; // UUID edits are handled by scan()
        e->meta = std::move(*meta);
    }
    (void)importLocked(*e, false, &changed);
}

usize AssetRegistry::poll() {
    if (!m_watcher) return 0;
    m_watcher->poll();
    std::vector<fs::path> pending;
    {
        std::lock_guard l(m_changeMutex);
        pending.swap(m_pendingChanges);
    }
    if (pending.empty()) return 0;
    std::vector<Uuid> changed;
    bool rescan = false;
    {
        std::lock_guard lock(m_mutex);
        for (const auto& p : pending) {
            std::error_code ec;
            const bool exists = fs::exists(p, ec);
            fs::path source = p;
            if (source.extension() == kMetaExtension) source.replace_extension();
            if (!exists || !m_pathToUuid.count(relativePath(source))) rescan = true;
        }
    }
    if (rescan) scan();
    {
        std::lock_guard lock(m_mutex);
        std::sort(pending.begin(), pending.end());
        pending.erase(std::unique(pending.begin(), pending.end()), pending.end());
        for (const auto& p : pending) {
            if (fs::exists(p)) handleChangeLocked(p, changed);
        }
    }
    std::sort(changed.begin(), changed.end());
    changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
    for (const Uuid& c : changed) {
        onReimported.emit(c);
        m_changed.emit(c);
    }
    return changed.size();
}

} // namespace ox::assets
