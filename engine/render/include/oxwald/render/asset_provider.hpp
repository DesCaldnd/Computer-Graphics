#pragma once

// Glue between the assets module's AssetManager and the GPU resource cache (requires Oxwald::assets).
//
//   renderer.resources().setProvider(ox::render::makeAssetManagerProvider(assetManager), &jobs);
//   auto hotReload = ox::render::connectAssetHotReload(assetManager, renderer.resources());  // keep alive

#include <oxwald/core/events.hpp>
#include <oxwald/render/gpu_resource_cache.hpp>

namespace ox::assets {
class AssetManager;
}

namespace ox::render {

// Loads meshes / textures / materials synchronously through AssetManager::loadSync (called on worker threads by the
// cache). Missing or failed assets resolve to nullptr (the cache then uses its placeholders).
AssetProvider makeAssetManagerProvider(assets::AssetManager& manager);

// AssetManager::onReloaded → GpuResourceCache::queueInvalidate (thread safe; applied in the next cache update()).
ScopedConnection connectAssetHotReload(assets::AssetManager& manager, GpuResourceCache& cache);

} // namespace ox::render
