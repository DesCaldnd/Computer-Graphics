#include <oxwald/gameplay/providers.hpp>

#include <algorithm>

namespace ox::gameplay {

void GameplayAssetRegistry::addMesh(const Uuid& id, MeshTriangles mesh) {
    m_meshes[id] = std::make_shared<const MeshTriangles>(std::move(mesh));
}
void GameplayAssetRegistry::addSkeleton(const Uuid& id, std::shared_ptr<const anim::Skeleton> s) { m_skeletons[id] = std::move(s); }
void GameplayAssetRegistry::addController(const Uuid& id, std::shared_ptr<const anim::AnimatorController> c) {
    m_controllers[id] = std::move(c);
}
void GameplayAssetRegistry::addClip(const Uuid& id, std::shared_ptr<const anim::AnimationClip> c) { m_clips[id] = std::move(c); }
void GameplayAssetRegistry::addBehaviorTree(const Uuid& id, nlohmann::json tree) { m_trees[id] = std::move(tree); }
void GameplayAssetRegistry::addPrefab(std::string name, serial::Document prefab) {
    m_prefabs[std::move(name)] = std::make_shared<const serial::Document>(std::move(prefab));
}
void GameplayAssetRegistry::addScript(std::string name, std::string source, const Uuid& id) {
    if (id.isValid()) m_scriptIds[id] = name;
    m_scripts[name] = ScriptSource{name, std::move(source), {}};
}
void GameplayAssetRegistry::addScriptFile(std::string name, std::string path, const Uuid& id) {
    if (id.isValid()) m_scriptIds[id] = name;
    m_scripts[name] = ScriptSource{name, {}, std::move(path)};
}
void GameplayAssetRegistry::addSound(const Uuid& id, audio::SoundId sound) { m_sounds[id] = sound; }

template <class Map, class Key>
static auto lookup(const Map& m, const Key& k) -> typename Map::mapped_type {
    auto it = m.find(k);
    return it == m.end() ? typename Map::mapped_type{} : it->second;
}

std::shared_ptr<const MeshTriangles> GameplayAssetRegistry::meshTriangles(const Uuid& id) { return lookup(m_meshes, id); }
std::shared_ptr<const anim::Skeleton> GameplayAssetRegistry::skeleton(const Uuid& id) { return lookup(m_skeletons, id); }
std::shared_ptr<const anim::AnimatorController> GameplayAssetRegistry::controller(const Uuid& id) {
    return lookup(m_controllers, id);
}
std::shared_ptr<const anim::AnimationClip> GameplayAssetRegistry::clip(const Uuid& id) { return lookup(m_clips, id); }

std::optional<nlohmann::json> GameplayAssetRegistry::behaviorTree(const Uuid& id) {
    auto it = m_trees.find(id);
    if (it == m_trees.end()) return std::nullopt;
    return it->second;
}

std::shared_ptr<const serial::Document> GameplayAssetRegistry::prefab(std::string_view nameOrPath) {
    auto it = m_prefabs.find(std::string(nameOrPath));
    return it == m_prefabs.end() ? nullptr : it->second;
}

std::vector<std::string> GameplayAssetRegistry::prefabNames() const {
    std::vector<std::string> names;
    names.reserve(m_prefabs.size());
    for (const auto& [name, doc] : m_prefabs) names.push_back(name);
    std::sort(names.begin(), names.end());
    return names;
}

std::optional<ScriptSource> GameplayAssetRegistry::scriptByName(std::string_view name) {
    auto it = m_scripts.find(std::string(name));
    if (it == m_scripts.end()) return std::nullopt;
    return it->second;
}

std::optional<ScriptSource> GameplayAssetRegistry::scriptById(const Uuid& id) {
    auto it = m_scriptIds.find(id);
    return it == m_scriptIds.end() ? std::nullopt : scriptByName(it->second);
}

audio::SoundId GameplayAssetRegistry::sound(audio::AudioEngine&, const Uuid& clip) { return lookup(m_sounds, clip); }

void GameplayAssetRegistry::registerIn(Services& services) {
    if (!services.has<IMeshColliderProvider>()) services.addExternal<IMeshColliderProvider>(*this);
    if (!services.has<IAnimationAssetProvider>()) services.addExternal<IAnimationAssetProvider>(*this);
    if (!services.has<IBehaviorTreeProvider>()) services.addExternal<IBehaviorTreeProvider>(*this);
    if (!services.has<IPrefabProvider>()) services.addExternal<IPrefabProvider>(*this);
    if (!services.has<IScriptSourceProvider>()) services.addExternal<IScriptSourceProvider>(*this);
    if (!services.has<IAudioClipProvider>()) services.addExternal<IAudioClipProvider>(*this);
}

} // namespace ox::gameplay
