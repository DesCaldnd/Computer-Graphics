#pragma once

#include <oxwald/animation/animator.hpp>
#include <oxwald/audio/audio_engine.hpp>
#include <oxwald/core/events.hpp>
#include <oxwald/core/serial/value.hpp>
#include <oxwald/core/services.hpp>
#include <oxwald/core/uuid.hpp>

#include <nlohmann/json.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Pluggable asset resolution. The assets module is being written in parallel, so every place where a
// component references an asset by UUID (or name) goes through one of these small interfaces, registered in
// ox::Services. GameplayAssetRegistry is an in-memory implementation of all of them (tests, tools, procedural
// content); the asset database will provide the real ones.
namespace ox::gameplay {

struct MeshTriangles {
    std::vector<glm::vec3> vertices;
    std::vector<u32> indices; // 3 per triangle, CCW front faces
};

// Triangle data for mesh colliders and navmesh baking (MeshRenderer meshes, ColliderComponent::mesh).
class IMeshColliderProvider {
public:
    virtual ~IMeshColliderProvider() = default;
    [[nodiscard]] virtual std::shared_ptr<const MeshTriangles> meshTriangles(const Uuid& mesh) = 0;
};

class IAnimationAssetProvider {
public:
    virtual ~IAnimationAssetProvider() = default;
    [[nodiscard]] virtual std::shared_ptr<const anim::Skeleton> skeleton(const Uuid& id) = 0;
    [[nodiscard]] virtual std::shared_ptr<const anim::AnimatorController> controller(const Uuid& id) = 0;
    [[nodiscard]] virtual std::shared_ptr<const anim::AnimationClip> clip(const Uuid& id) = 0;
};

// Behaviour tree definitions (BTFactory JSON).
class IBehaviorTreeProvider {
public:
    virtual ~IBehaviorTreeProvider() = default;
    [[nodiscard]] virtual std::optional<nlohmann::json> behaviorTree(const Uuid& id) = 0;
};

// Prefab documents by name / path (scene.spawn, network spawning) or id.
class IPrefabProvider {
public:
    virtual ~IPrefabProvider() = default;
    [[nodiscard]] virtual std::shared_ptr<const serial::Document> prefab(std::string_view nameOrPath) = 0;
    // Names usable as network types (clients register them with the replication factory).
    [[nodiscard]] virtual std::vector<std::string> prefabNames() const { return {}; }
};

struct ScriptSource {
    std::string name;
    std::string source; // empty => use `path`
    std::string path;
};

// Script sources by name (ScriptComponent::script) or asset id (ScriptComponent::asset).
class IScriptSourceProvider {
public:
    virtual ~IScriptSourceProvider() = default;
    [[nodiscard]] virtual std::optional<ScriptSource> scriptByName(std::string_view name) = 0;
    [[nodiscard]] virtual std::optional<ScriptSource> scriptById(const Uuid& id) = 0;
};

class IAudioClipProvider {
public:
    virtual ~IAudioClipProvider() = default;
    // Invalid SoundId when unknown. May load lazily into `engine`.
    [[nodiscard]] virtual audio::SoundId sound(audio::AudioEngine& engine, const Uuid& clip) = 0;
};

// Normalized height samples of a heightmap asset (square grid, row-major, z * resolution + x). World scale,
// origin and storage format come from the consumer (TerrainComponent).
struct HeightmapData {
    u32 resolution = 0;
    std::vector<f32> normalized;
};

class IHeightmapProvider {
public:
    virtual ~IHeightmapProvider() = default;
    [[nodiscard]] virtual std::shared_ptr<const HeightmapData> heightmap(const Uuid& id) = 0;
};

// ---- hot reload ---------------------------------------------------------------------------------------------

enum class GameplayAssetKind : u8 {
    Mesh,
    Skeleton,
    AnimatorController,
    AnimationClip,
    BehaviorTree,
    Prefab,
    Script,
    AudioClip,
    Heightmap,
    Other,
};

struct GameplayAssetChange {
    GameplayAssetKind kind = GameplayAssetKind::Other;
    Uuid id;
    std::string path; // asset path when known ("Scripts/player.lua"), also matched against names
};

// Service: providers backed by a changing source (the asset database) emit `changed` on the main thread after an
// asset was reloaded; the gameplay runtimes propagate it to running instances at the next frame
// (scripts hot reload in place, behaviour trees are rebuilt, prefab instances re-synchronised, ...).
class GameplayAssetEvents {
public:
    Signal<const GameplayAssetChange&> changed;
};

// In-memory implementation of every provider interface.
class GameplayAssetRegistry final : public IMeshColliderProvider,
                                    public IAnimationAssetProvider,
                                    public IBehaviorTreeProvider,
                                    public IPrefabProvider,
                                    public IScriptSourceProvider,
                                    public IAudioClipProvider {
public:
    void addMesh(const Uuid& id, MeshTriangles mesh);
    void addSkeleton(const Uuid& id, std::shared_ptr<const anim::Skeleton> skeleton);
    void addController(const Uuid& id, std::shared_ptr<const anim::AnimatorController> controller);
    void addClip(const Uuid& id, std::shared_ptr<const anim::AnimationClip> clip);
    void addBehaviorTree(const Uuid& id, nlohmann::json tree);
    void addPrefab(std::string name, serial::Document prefab);
    void addScript(std::string name, std::string source, const Uuid& id = {});
    void addScriptFile(std::string name, std::string path, const Uuid& id = {});
    void addSound(const Uuid& id, audio::SoundId sound);

    std::shared_ptr<const MeshTriangles> meshTriangles(const Uuid& mesh) override;
    std::shared_ptr<const anim::Skeleton> skeleton(const Uuid& id) override;
    std::shared_ptr<const anim::AnimatorController> controller(const Uuid& id) override;
    std::shared_ptr<const anim::AnimationClip> clip(const Uuid& id) override;
    std::optional<nlohmann::json> behaviorTree(const Uuid& id) override;
    std::shared_ptr<const serial::Document> prefab(std::string_view nameOrPath) override;
    std::vector<std::string> prefabNames() const override;
    std::optional<ScriptSource> scriptByName(std::string_view name) override;
    std::optional<ScriptSource> scriptById(const Uuid& id) override;
    audio::SoundId sound(audio::AudioEngine& engine, const Uuid& clip) override;

    // Registers this object (not owned) under every provider interface that is not registered yet.
    void registerIn(Services& services);

private:
    std::unordered_map<Uuid, std::shared_ptr<const MeshTriangles>> m_meshes;
    std::unordered_map<Uuid, std::shared_ptr<const anim::Skeleton>> m_skeletons;
    std::unordered_map<Uuid, std::shared_ptr<const anim::AnimatorController>> m_controllers;
    std::unordered_map<Uuid, std::shared_ptr<const anim::AnimationClip>> m_clips;
    std::unordered_map<Uuid, nlohmann::json> m_trees;
    std::unordered_map<std::string, std::shared_ptr<const serial::Document>> m_prefabs;
    std::unordered_map<std::string, ScriptSource> m_scripts;
    std::unordered_map<Uuid, std::string> m_scriptIds;
    std::unordered_map<Uuid, audio::SoundId> m_sounds;
};

} // namespace ox::gameplay
