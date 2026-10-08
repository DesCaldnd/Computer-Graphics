#pragma once

// Gameplay asset providers backed by the asset database (assets::AssetManager over an AssetRegistry in the
// editor/dev builds or a PakAssetSource in cooked games). Compiled when the assets module is configured
// (OX_GAMEPLAY_HAS_ASSETS=1).
//
//   ox::assets::AssetManager manager(registry, &jobs);
//   ox::gameplay::AssetProviders providers(manager);
//   providers.registerIn(services);          // every provider interface + GameplayAssetEvents (hot reload)
//
// Dependency direction: gameplay -> assets. The asset database knows nothing about gameplay; gameplay owns the
// provider interfaces (providers.hpp) and this adapter implements them on top of AssetManager.

#include <oxwald/assets/asset_manager.hpp>
#include <oxwald/assets/importer.hpp>
#include <oxwald/gameplay/providers.hpp>

#include <mutex>
#include <string_view>
#include <unordered_map>

namespace ox::gameplay {

// Gameplay asset kinds stored as AssetType::Raw blobs, told apart by the blob info "kind":
//   *.oxbt        behaviour tree (BTFactory JSON)                    info {"kind": "BehaviorTree"}
//   *.oxanimctrl  animator controller (InlineAnimatorController JSON) info {"kind": "AnimatorController"}
// Raw keeps them cookable into paks without new AssetType values.
inline constexpr std::string_view kBehaviorTreeAssetKind = "BehaviorTree";
inline constexpr std::string_view kAnimatorControllerAssetKind = "AnimatorController";

// Adds the gameplay importers (behaviour trees, animator controllers) to an AssetRegistry's importer registry.
void registerGameplayImporters(assets::ImporterRegistry& importers);

class AssetProviders final : public IMeshColliderProvider,
                             public IAnimationAssetProvider,
                             public IBehaviorTreeProvider,
                             public IPrefabProvider,
                             public IScriptSourceProvider,
                             public IAudioClipProvider,
                             public IHeightmapProvider {
public:
    explicit AssetProviders(assets::AssetManager& manager);
    ~AssetProviders() override;
    AssetProviders(const AssetProviders&) = delete;
    AssetProviders& operator=(const AssetProviders&) = delete;

    // Registers this object (not owned) under every provider interface that is not registered yet, and a
    // GameplayAssetEvents service (created when missing) that receives the hot reload notifications.
    void registerIn(Services& services);
    [[nodiscard]] assets::AssetManager& manager() { return m_manager; }

    // Resolves an asset reference used by gameplay data: UUID string, asset path ("Prefabs/enemy.oxprefab"),
    // path without extension ("Prefabs/enemy") or a unique file stem ("enemy"). Invalid Uuid when unknown.
    [[nodiscard]] Uuid resolve(std::string_view nameOrPath, assets::AssetType type);

    // Provider interfaces. Loads are synchronous (AssetManager::loadSync) on first use and cached; handles are kept
    // so the assets stay resident while gameplay uses them. Must be called on the game thread.
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
    std::shared_ptr<const HeightmapData> heightmap(const Uuid& id) override;

    // Drops every cached asset (they are reloaded on next use).
    void clearCache();

private:
    struct NameIndex;
    void onReloaded(const Uuid& id, assets::AssetType type);
    void invalidate(const Uuid& id);
    void rebuildIndex() const;
    [[nodiscard]] std::optional<std::shared_ptr<const assets::BlobAsset>> rawBlob(const Uuid& id, std::string_view kind);

    assets::AssetManager& m_manager;
    GameplayAssetEvents* m_events = nullptr;
    std::unique_ptr<GameplayAssetEvents> m_ownedEvents;
    ScopedConnection m_reloadConnection;

    std::unordered_map<Uuid, assets::UntypedAssetHandle> m_handles; // keeps used assets resident
    std::unordered_map<Uuid, std::shared_ptr<const MeshTriangles>> m_meshes;
    std::unordered_map<Uuid, std::shared_ptr<const anim::AnimatorController>> m_controllers;
    std::unordered_map<Uuid, std::shared_ptr<const HeightmapData>> m_heightmaps;
    std::unordered_map<const audio::AudioEngine*, std::unordered_map<Uuid, audio::SoundId>> m_sounds;
    mutable std::unique_ptr<NameIndex> m_index;
};

} // namespace ox::gameplay
