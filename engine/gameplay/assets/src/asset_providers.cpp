#include <oxwald/core/log.hpp>
#include <oxwald/core/serial/format.hpp>
#include <oxwald/gameplay/animation.hpp>
#include <oxwald/gameplay/asset_providers.hpp>

#include <oxwald/assets/mesh.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>

namespace ox::gameplay {

namespace {

using assets::AssetType;

std::string_view asText(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::vector<std::byte> toBytes(std::string_view text) {
    const auto* p = reinterpret_cast<const std::byte*>(text.data());
    return {p, p + text.size()};
}

// JSON source -> Raw blob tagged with the gameplay kind.
class JsonBlobImporter final : public assets::IAssetImporter {
public:
    JsonBlobImporter(std::string name, std::string_view kind, std::string extension)
        : m_name(std::move(name)), m_kind(kind), m_extension(std::move(extension)) {}
    std::string_view name() const override { return m_name; }
    u32 version() const override { return 1; }
    std::vector<std::string> extensions() const override { return {m_extension}; }
    AssetType mainType() const override { return AssetType::Raw; }
    Status import(assets::ImportContext& ctx) override {
        auto bytes = ctx.readSource();
        if (!bytes) return bytes.error();
        auto json = nlohmann::json::parse(asText(*bytes), nullptr, false, true);
        if (json.is_discarded() || !json.is_object()) return makeError("{}: not a JSON object", ctx.assetPath());
        std::vector<Uuid> deps;
        if (m_kind == kAnimatorControllerAssetKind && json.contains("states") && json["states"].is_array()) {
            for (const auto& s : json["states"]) {
                if (!s.is_object() || !s.contains("clip") || !s["clip"].is_string()) continue;
                if (auto id = Uuid::parse(s["clip"].get<std::string>()); id && id->isValid()) deps.push_back(*id);
            }
        }
        nlohmann::ordered_json info;
        info["kind"] = m_kind;
        const std::string text = json.dump();
        auto& a = ctx.setMain(AssetType::Raw, assets::serializeBlob(AssetType::Raw, info, toBytes(text)));
        a.info = info;
        a.dependencies = std::move(deps);
        return {};
    }

private:
    std::string m_name;
    std::string m_kind;
    std::string m_extension;
};

std::string stripExtension(std::string_view path) {
    // "Models/hero.glb" -> "Models/hero", ".oxprefab.json" counts as one extension.
    const auto slash = path.find_last_of('/');
    const auto dot = path.find('.', slash == std::string_view::npos ? 0 : slash + 1);
    return std::string(dot == std::string_view::npos || dot == 0 ? path : path.substr(0, dot));
}

std::string stem(std::string_view path) {
    const std::string noExt = stripExtension(path);
    const auto slash = noExt.find_last_of('/');
    return slash == std::string::npos ? noExt : noExt.substr(slash + 1);
}

GameplayAssetKind kindOf(AssetType type) {
    switch (type) {
    case AssetType::Mesh: return GameplayAssetKind::Mesh;
    case AssetType::Skeleton: return GameplayAssetKind::Skeleton;
    case AssetType::AnimationClip: return GameplayAssetKind::AnimationClip;
    case AssetType::Prefab: return GameplayAssetKind::Prefab;
    case AssetType::Script: return GameplayAssetKind::Script;
    case AssetType::Audio: return GameplayAssetKind::AudioClip;
    case AssetType::Heightmap: return GameplayAssetKind::Heightmap;
    default: return GameplayAssetKind::Other;
    }
}

} // namespace

namespace {
// AssetManager hands out an existing slot even when the requested type does not match (it only warns), so the
// providers check the record first: a script id passed where a behaviour tree is expected must not alias data.
template <class T>
assets::AssetHandle<T> loadTyped(assets::AssetManager& manager, const Uuid& id) {
    if (!id.isValid()) return {};
    auto rec = manager.source().record(id);
    if (!rec || rec->type != T::kAssetType) return {};
    return manager.loadSync<T>(id);
}
} // namespace

void registerGameplayImporters(assets::ImporterRegistry& importers) {
    importers.add(std::make_unique<JsonBlobImporter>("behavior_tree", kBehaviorTreeAssetKind, ".oxbt"));
    importers.add(std::make_unique<JsonBlobImporter>("animator_controller", kAnimatorControllerAssetKind, ".oxanimctrl"));
}

// Name lookup over the asset source: path, path without extension, unique stem.
struct AssetProviders::NameIndex {
    struct Key {
        std::string name;
        AssetType type;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        usize operator()(const Key& k) const { return std::hash<std::string>{}(k.name) ^ (usize(k.type) * 0x9E3779B97F4A7C15ull); }
    };
    std::unordered_map<Key, Uuid, KeyHash> byName; // nil Uuid = ambiguous stem
    std::vector<std::string> prefabNames;
    std::chrono::steady_clock::time_point built{};
    bool valid = false;
};

AssetProviders::AssetProviders(assets::AssetManager& manager)
    : m_manager(manager), m_index(std::make_unique<NameIndex>()) {
    m_reloadConnection = m_manager.onReloaded.connect([this](const Uuid& id, AssetType type) { onReloaded(id, type); });
}

AssetProviders::~AssetProviders() = default;

void AssetProviders::registerIn(Services& services) {
    if (!services.has<IMeshColliderProvider>()) services.addExternal<IMeshColliderProvider>(*this);
    if (!services.has<IAnimationAssetProvider>()) services.addExternal<IAnimationAssetProvider>(*this);
    if (!services.has<IBehaviorTreeProvider>()) services.addExternal<IBehaviorTreeProvider>(*this);
    if (!services.has<IPrefabProvider>()) services.addExternal<IPrefabProvider>(*this);
    if (!services.has<IScriptSourceProvider>()) services.addExternal<IScriptSourceProvider>(*this);
    if (!services.has<IAudioClipProvider>()) services.addExternal<IAudioClipProvider>(*this);
    if (!services.has<IHeightmapProvider>()) services.addExternal<IHeightmapProvider>(*this);
    if (auto* existing = services.tryGet<GameplayAssetEvents>()) {
        m_events = existing;
    } else {
        // Owned here so the service outlives nothing it should not: registered as external, freed with us.
        m_ownedEvents = std::make_unique<GameplayAssetEvents>();
        m_events = m_ownedEvents.get();
        services.addExternal<GameplayAssetEvents>(*m_events);
    }
}

void AssetProviders::rebuildIndex() const {
    NameIndex& idx = *m_index;
    idx.byName.clear();
    idx.prefabNames.clear();
    std::unordered_map<NameIndex::Key, u32, NameIndex::KeyHash> stemCount;
    auto& source = m_manager.source();
    for (const Uuid& id : source.allAssets()) {
        auto rec = source.record(id);
        if (!rec || rec->path.empty()) continue;
        idx.byName[{rec->path, rec->type}] = id;
        const std::string noExt = stripExtension(rec->path);
        if (noExt != rec->path) idx.byName.try_emplace({noExt, rec->type}, id);
        const std::string s = stem(rec->path);
        if (rec->path.find('#') == std::string::npos && ++stemCount[{s, rec->type}] == 1) {
            idx.byName.try_emplace({s, rec->type}, id);
        } else if (rec->path.find('#') == std::string::npos) {
            auto it = idx.byName.find({s, rec->type});
            if (it != idx.byName.end() && it->second != id && s != rec->path && s != noExt) it->second = Uuid{};
        }
        if (rec->type == AssetType::Prefab && rec->path.find('#') == std::string::npos) idx.prefabNames.push_back(noExt);
    }
    for (const auto& [key, count] : stemCount) {
        if (count == 1 && key.type == AssetType::Prefab) {
            if (std::find(idx.prefabNames.begin(), idx.prefabNames.end(), key.name) == idx.prefabNames.end()) {
                idx.prefabNames.push_back(key.name);
            }
        }
    }
    std::sort(idx.prefabNames.begin(), idx.prefabNames.end());
    idx.built = std::chrono::steady_clock::now();
    idx.valid = true;
}

Uuid AssetProviders::resolve(std::string_view nameOrPath, AssetType type) {
    if (nameOrPath.empty()) return {};
    if (auto id = Uuid::parse(nameOrPath); id && id->isValid()) return *id;
    if (auto id = m_manager.source().uuidForPath(nameOrPath)) return *id;
    auto lookup = [&]() -> std::optional<Uuid> {
        auto it = m_index->byName.find({std::string(nameOrPath), type});
        if (it == m_index->byName.end()) return std::nullopt;
        return it->second;
    };
    if (!m_index->valid) rebuildIndex();
    auto found = lookup();
    // New assets appear while the editor runs: rescan on a miss, at most once per second.
    if (!found && std::chrono::steady_clock::now() - m_index->built > std::chrono::seconds(1)) {
        rebuildIndex();
        found = lookup();
    }
    if (found && !found->isValid()) OX_LOG_WARN("gameplay", "asset name '{}' is ambiguous; use its path", nameOrPath);
    return found.value_or(Uuid{});
}

std::shared_ptr<const MeshTriangles> AssetProviders::meshTriangles(const Uuid& id) {
    if (auto it = m_meshes.find(id); it != m_meshes.end()) return it->second;
    auto handle = loadTyped<assets::MeshData>(m_manager, id);
    const assets::MeshData* mesh = handle.get();
    if (!mesh) return nullptr;
    auto tris = std::make_shared<MeshTriangles>();
    if (!mesh->collision.vertices.empty()) {
        // Simplified, welded collision mesh produced at import (generateCollision).
        tris->vertices = mesh->collision.vertices;
        tris->indices = mesh->collision.indices;
    } else {
        tris->vertices = mesh->positions;
        for (const auto& sm : mesh->submeshes) {
            if (sm.lods.empty()) continue;
            const auto& lod = sm.lods.front();
            tris->indices.insert(tris->indices.end(), mesh->indices.begin() + lod.indexOffset,
                                 mesh->indices.begin() + lod.indexOffset + lod.indexCount);
        }
    }
    m_handles[id] = handle;
    return m_meshes[id] = std::move(tris);
}

std::shared_ptr<const anim::Skeleton> AssetProviders::skeleton(const Uuid& id) {
#if OX_ASSETS_HAS_ANIMATION
    auto handle = loadTyped<assets::SkeletonAsset>(m_manager, id);
    auto data = handle.share();
    if (!data) return nullptr;
    m_handles[id] = handle;
    return {data, &data->skeleton}; // aliasing: keeps the asset alive
#else
    return nullptr;
#endif
}

std::shared_ptr<const anim::AnimationClip> AssetProviders::clip(const Uuid& id) {
#if OX_ASSETS_HAS_ANIMATION
    auto handle = loadTyped<assets::AnimationClipAsset>(m_manager, id);
    auto data = handle.share();
    if (!data) return nullptr;
    m_handles[id] = handle;
    return {data, &data->clip};
#else
    return nullptr;
#endif
}

std::optional<std::shared_ptr<const assets::BlobAsset>> AssetProviders::rawBlob(const Uuid& id, std::string_view kind) {
    auto handle = loadTyped<assets::RawAsset>(m_manager, id);
    std::shared_ptr<const assets::BlobAsset> data = handle.share();
    if (!data) return std::nullopt;
    if (data->info.value("kind", std::string()) != kind) {
        OX_LOG_WARN("gameplay", "asset {} is not a {} (kind '{}')", id.toString(), kind, data->info.value("kind", std::string()));
        return std::nullopt;
    }
    m_handles[id] = handle;
    return data;
}

std::shared_ptr<const anim::AnimatorController> AssetProviders::controller(const Uuid& id) {
    if (auto it = m_controllers.find(id); it != m_controllers.end()) return it->second;
    auto blob = rawBlob(id, kAnimatorControllerAssetKind);
    if (!blob) return nullptr;
    auto json = nlohmann::ordered_json::parse(asText((*blob)->data), nullptr, false, true);
    InlineAnimatorController desc;
    auto doc = json.is_discarded() ? Result<serial::Document>(makeError("invalid JSON")) : serial::decodeJson(json);
    if (!doc || !serial::fromValue(doc->root, desc)) {
        OX_LOG_WARN("gameplay", "animator controller {} could not be parsed", id.toString());
        return nullptr;
    }
    return m_controllers[id] = buildAnimatorController(desc, this);
}

std::optional<nlohmann::json> AssetProviders::behaviorTree(const Uuid& id) {
    auto blob = rawBlob(id, kBehaviorTreeAssetKind);
    if (!blob) return std::nullopt;
    auto json = nlohmann::json::parse(asText((*blob)->data), nullptr, false, true);
    if (json.is_discarded()) return std::nullopt;
    return json;
}

std::shared_ptr<const serial::Document> AssetProviders::prefab(std::string_view nameOrPath) {
    const Uuid id = resolve(nameOrPath, AssetType::Prefab);
    if (!id.isValid()) return nullptr;
    auto handle = loadTyped<assets::PrefabAsset>(m_manager, id);
    auto data = handle.share();
    if (!data) return nullptr;
    m_handles[id] = handle;
    return {data, &data->document};
}

std::vector<std::string> AssetProviders::prefabNames() const {
    rebuildIndex();
    return m_index->prefabNames;
}

std::optional<ScriptSource> AssetProviders::scriptById(const Uuid& id) {
    auto handle = loadTyped<assets::ScriptAsset>(m_manager, id);
    const assets::ScriptAsset* script = handle.get();
    if (!script) return std::nullopt;
    m_handles[id] = handle;
    return ScriptSource{script->path.empty() ? id.toString() : script->path, script->source, {}};
}

std::optional<ScriptSource> AssetProviders::scriptByName(std::string_view name) {
    const Uuid id = resolve(name, AssetType::Script);
    if (!id.isValid()) return std::nullopt;
    return scriptById(id);
}

audio::SoundId AssetProviders::sound(audio::AudioEngine& engine, const Uuid& id) {
    auto& perEngine = m_sounds[&engine];
    if (auto it = perEngine.find(id); it != perEngine.end()) return it->second;
    auto handle = loadTyped<assets::AudioClipAsset>(m_manager, id);
    const assets::AudioClipAsset* clip = handle.get();
    if (!clip) return {};
    const audio::SoundId sound = engine.loadSoundFromMemory(clip->data, id.toString());
    if (sound.valid()) perEngine[id] = sound;
    return sound;
}

std::shared_ptr<const HeightmapData> AssetProviders::heightmap(const Uuid& id) {
    if (auto it = m_heightmaps.find(id); it != m_heightmaps.end()) return it->second;
    auto handle = loadTyped<assets::HeightmapAsset>(m_manager, id);
    const assets::HeightmapAsset* hm = handle.get();
    if (!hm) return nullptr;
    const u32 w = hm->info.value("width", 0u);
    const u32 h = hm->info.value("height", 0u);
    const std::string format = hm->info.value("format", std::string("r16"));
    const usize bpp = format == "r8" ? 1 : format == "r32f" ? 4 : 2;
    if (w == 0 || w != h || hm->data.size() != usize(w) * h * bpp) {
        OX_LOG_WARN("gameplay", "heightmap {}: {}x{} {} is not a square grid matching its data", id.toString(), w, h, format);
        return nullptr;
    }
    auto out = std::make_shared<HeightmapData>();
    out->resolution = w;
    out->normalized.resize(usize(w) * h);
    const std::byte* src = hm->data.data();
    for (usize i = 0; i < out->normalized.size(); ++i) {
        if (bpp == 1) {
            out->normalized[i] = f32(u8(src[i])) / 255.f;
        } else if (bpp == 2) {
            u16 v;
            std::memcpy(&v, src + i * 2, 2);
            out->normalized[i] = f32(v) / 65535.f;
        } else {
            std::memcpy(&out->normalized[i], src + i * 4, 4);
        }
    }
    m_handles[id] = handle;
    return m_heightmaps[id] = std::move(out);
}

void AssetProviders::invalidate(const Uuid& id) {
    m_meshes.erase(id);
    m_controllers.erase(id);
    m_heightmaps.erase(id);
    for (auto& [engine, sounds] : m_sounds) sounds.erase(id); // new SoundId on next use (old voices keep playing)
}

void AssetProviders::clearCache() {
    m_meshes.clear();
    m_controllers.clear();
    m_heightmaps.clear();
    m_sounds.clear();
    m_handles.clear();
    m_index->valid = false;
}

void AssetProviders::onReloaded(const Uuid& id, AssetType type) {
    invalidate(id);
    m_index->valid = false;
    GameplayAssetChange change;
    change.id = id;
    change.kind = kindOf(type);
    if (type == AssetType::Raw) {
        if (auto data = m_manager.find(id).dataUntyped()) {
            const std::string kind = static_cast<const assets::BlobAsset*>(data.get())->info.value("kind", std::string());
            if (kind == kBehaviorTreeAssetKind) change.kind = GameplayAssetKind::BehaviorTree;
            if (kind == kAnimatorControllerAssetKind) change.kind = GameplayAssetKind::AnimatorController;
        }
    }
    // Controllers resolve their clips when built.
    if (change.kind == GameplayAssetKind::AnimationClip) m_controllers.clear();
    if (auto rec = m_manager.source().record(id)) change.path = rec->path;
    if (m_events) m_events->changed.emit(change);
}

} // namespace ox::gameplay
