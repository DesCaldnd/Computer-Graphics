#include <oxwald/assets/asset_manager.hpp>
#include <oxwald/render/asset_provider.hpp>

namespace ox::render {

namespace {
template <class T>
std::shared_ptr<const T> loadShared(assets::AssetManager& m, const Uuid& id) {
    auto h = m.loadSync<T>(id);
    return h.isLoaded() ? h.share() : nullptr;
}
} // namespace

AssetProvider makeAssetManagerProvider(assets::AssetManager& manager) {
    AssetProvider p;
    p.loadMesh = [&manager](const Uuid& id) { return loadShared<assets::MeshData>(manager, id); };
    p.loadTexture = [&manager](const Uuid& id) { return loadShared<assets::TextureData>(manager, id); };
    p.loadMaterial = [&manager](const Uuid& id) { return loadShared<assets::MaterialAsset>(manager, id); };
    return p;
}

ScopedConnection connectAssetHotReload(assets::AssetManager& manager, GpuResourceCache& cache) {
    return ScopedConnection(manager.onReloaded.connect([&cache](const Uuid& id, assets::AssetType) { cache.queueInvalidate(id); }));
}

} // namespace ox::render
