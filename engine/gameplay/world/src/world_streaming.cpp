#include "world_runtime_impl.hpp"

#include <oxwald/core/profile.hpp>
#include <oxwald/scene/prefab.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>

namespace ox::gameplay {

namespace {

// nullopt = no such chunk (empty area); empty optional vector never returned for an existing file.
std::optional<std::vector<u8>> readChunkFile(Vfs* vfs, const std::string& file, bool& error) {
    error = false;
    if (vfs && file.find("://") != std::string::npos) {
        if (!vfs->exists(file)) return std::nullopt;
        auto bytes = vfs->readBytes(file);
        if (!bytes) {
            error = true;
            return std::nullopt;
        }
        std::vector<u8> out(bytes->size());
        std::memcpy(out.data(), bytes->data(), out.size());
        return out;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) return std::nullopt;
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = true;
        return std::nullopt;
    }
    std::vector<u8> out((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return out;
}

} // namespace

void WorldRuntime::Impl::stopStreaming(StreamingState& st) {
    if (st.streamer) {
        st.streamer->unloadAll(); // onUnload destroys the spawned entities
        st.streamer.reset();
    }
    if (world) {
        for (auto& [c, list] : st.spawned) {
            for (auto h : list) {
                if (world->valid(h)) world->destroy(world->wrap(h));
            }
        }
    }
    st.spawned.clear();
}

void WorldRuntime::Impl::createStreamer(Entity e, StreamingState& st) {
    const auto& c = e.get<WorldStreamingComponent>();
    world::ChunkCallbacks cb;
    cb.load = [path = c.chunkPath, prefab = c.prefabPattern, v = vfs](world::ChunkCoord coord, const std::atomic<bool>& cancelled)
        -> std::unique_ptr<world::ChunkPayload> {
        auto out = std::make_unique<StreamedChunk>();
        if (!path.empty() && !cancelled.load()) {
            const std::string file = formatChunkPattern(path, coord);
            bool error = false;
            auto bytes = readChunkFile(v, file, error);
            if (error) {
                OX_LOG_WARN("gameplay.world", "chunk {}: read failed", file);
                return nullptr; // retried after failedRetryUpdates
            }
            if (bytes) {
                auto data = std::make_unique<world::ChunkData>();
                std::string err;
                if (!world::deserializeChunk(*bytes, *data, &err)) {
                    OX_LOG_WARN("gameplay.world", "chunk {}: {}", file, err);
                    return nullptr;
                }
                out->data = std::move(data);
            }
        }
        if (!prefab.empty()) out->prefabName = formatChunkPattern(prefab, coord);
        return out;
    };
    const entt::entity owner = e.handle();
    cb.onLoaded = [this, owner](world::ChunkCoord coord, world::ChunkPayload& p) { onChunkLoaded(owner, coord, p); };
    cb.onUnload = [this, owner](world::ChunkCoord coord, world::ChunkPayload&) { onChunkUnload(owner, coord); };

    world::ChunkStreamerSettings s = c.settings;
    s.chunkSize = std::max(1.f, s.chunkSize);
    s.unloadRadius = std::max(s.unloadRadius, s.loadRadius);
    st.streamer = std::make_unique<world::ChunkStreamer>(s, std::move(cb), executor);
}

void WorldRuntime::Impl::onChunkLoaded(entt::entity owner, world::ChunkCoord coord, world::ChunkPayload& payload) {
    if (!world || !world->valid(owner)) return;
    auto sit = streaming.find(owner);
    const auto* comp = world->registry().try_get<WorldStreamingComponent>(owner);
    if (sit == streaming.end() || !comp) return;
    auto& p = static_cast<StreamedChunk&>(payload);
    // Copy the template first: creating entities may grow component pools.
    const WorldStreamingComponent sc = *comp;
    const Entity parent = world->wrap(owner);
    const glm::vec2 origin = glm::vec2(f32(coord.x), f32(coord.z)) * std::max(1.f, sc.settings.chunkSize);
    std::vector<entt::entity> spawned;

    if (p.data && p.data->heightfield && p.data->heightfield->valid()) {
        ExternalTerrainData ext;
        ext.heightfield = std::make_shared<world::Heightfield>(std::move(*p.data->heightfield));
        if (p.data->splat) ext.splat = std::make_shared<world::SplatMap>(std::move(*p.data->splat));
        ext.vegetation = std::move(p.data->vegetation);
        ext.vegetationLayers = sc.vegetationLayers;
        const glm::vec2 o = ext.heightfield->desc().origin;
        Entity tile = world->create(std::format("Chunk {} {}", coord.x, coord.z), parent);
        tile.setWorldPosition({o.x, 0.f, o.y});
        self.setExternalTerrain(tile, std::move(ext));
        TerrainComponent tc;
        tc.source = TerrainSource::External;
        tc.centered = false;
        tc.lod = sc.tileLod;
        tc.collision = sc.tileCollision;
        tc.physicsTileQuads = sc.tilePhysicsQuads;
        tc.layers = sc.tileLayers;
        tile.add<TerrainComponent>(std::move(tc));
        spawned.push_back(tile.handle());
    }
    if (!p.prefabName.empty() && prefabs) {
        if (auto doc = prefabs->prefab(p.prefabName)) {
            auto root = instantiatePrefab(*world, *doc, parent);
            if (root) {
                root->setWorldPosition({origin.x, 0.f, origin.y});
                spawned.push_back(root->handle());
            } else {
                OX_LOG_WARN("gameplay.world", "chunk prefab '{}': {}", p.prefabName, root.error().message);
            }
        }
    }
    if (spawned.empty()) return;
    auto& list = sit->second.spawned[coord];
    list.insert(list.end(), spawned.begin(), spawned.end());
}

void WorldRuntime::Impl::onChunkUnload(entt::entity owner, world::ChunkCoord coord) {
    auto sit = streaming.find(owner);
    if (sit == streaming.end()) return;
    auto it = sit->second.spawned.find(coord);
    if (it == sit->second.spawned.end()) return;
    if (world) {
        for (auto h : it->second) {
            if (world->valid(h)) world->destroy(world->wrap(h));
        }
    }
    sit->second.spawned.erase(it);
}

void WorldRuntime::updateStreaming() {
    OX_PROFILE_ZONE();
    if (!m->world || !m->config.streaming || !m->playing) return;
    entt::registry& r = m->world->registry();
    std::vector<entt::entity> list;
    for (auto e : r.view<WorldStreamingComponent>()) list.push_back(e);
    std::sort(list.begin(), list.end());
    for (auto e : list) {
        if (!r.valid(e) || !r.all_of<WorldStreamingComponent>(e)) continue;
        StreamingState& st = m->streaming[e];
        const auto& c = r.get<WorldStreamingComponent>(e);
        if (!c.enabled || r.all_of<PendingDestroyTag>(e)) {
            m->stopStreaming(st);
            continue;
        }
        const bool useCamera = c.useCameraAsViewer;
        if (st.dirty || !st.streamer) {
            m->stopStreaming(st);
            m->createStreamer(m->world->wrap(e), st);
            st.dirty = false;
        }
        const std::vector<world::StreamingViewer> viewers = m->viewers(useCamera);
        st.streamer->update(viewers);
        if (auto* mc = r.try_get<WorldStreamingComponent>(e)) mc->loadedChunks = st.streamer->stats().loaded;
    }
}

} // namespace ox::gameplay
