#include "world_runtime_impl.hpp"

#include <oxwald/core/profile.hpp>
#include <oxwald/gameplay/common.hpp>
#include <oxwald/world/physics_bridge.hpp>
#include <oxwald/world/terrain_gen.hpp>

#include <algorithm>

namespace ox::gameplay {

std::shared_ptr<const world::TerrainGridMesh> WorldRuntime::Impl::gridMesh(u32 dim, u64& version) {
    auto& slot = gridMeshes[dim];
    if (!slot) {
        slot = std::make_shared<const world::TerrainGridMesh>(world::generateTerrainGrid(dim, true));
        ++gridMeshCounter;
    }
    // The version identifies the mesh object: stable for a given dimension.
    version = (u64(dim) << 32) | 1u;
    return slot;
}

void WorldRuntime::Impl::buildTerrain(Entity e, TerrainState& st) {
    OX_PROFILE_ZONE();
    const auto& c = e.get<TerrainComponent>();
    const glm::vec3 pos = e.worldPosition();
    st.builtPosition = pos;
    st.dirty = false;

    std::shared_ptr<world::Heightfield> hf;
    std::shared_ptr<world::SplatMap> splat;
    if (c.source == TerrainSource::External) {
        if (!st.hasExternal || !st.external.heightfield || !st.external.heightfield->valid()) {
            // Data not provided (yet): keep the previous build, nothing to show otherwise.
            if (!st.hf) return;
        } else {
            hf = st.external.heightfield;
            splat = st.external.splat;
        }
    } else {
        world::HeightfieldDesc d;
        d.resolution = std::max(2u, c.resolution);
        d.worldSize = std::max(1e-3f, c.worldSize);
        d.heightScale = c.heightScale;
        d.heightOffset = c.heightOffset + pos.y;
        d.format = c.format;
        std::shared_ptr<const HeightmapData> asset;
        if (c.source == TerrainSource::Heightmap) {
            asset = heightmaps && c.heightmap.isValid() ? heightmaps->heightmap(c.heightmap) : nullptr;
            if (asset && asset->resolution >= 2 && asset->normalized.size() == usize(asset->resolution) * asset->resolution) {
                d.resolution = asset->resolution;
            } else {
                OX_LOG_WARN("gameplay.world", "'{}': heightmap {} unavailable, using a flat terrain", e.name(),
                            c.heightmap.toString());
                asset.reset();
            }
        }
        const glm::vec2 xz{pos.x, pos.z};
        d.origin = c.centered ? xz - glm::vec2(d.worldSize * 0.5f) : xz + c.offset;
        hf = std::make_shared<world::Heightfield>(d);
        if (asset) {
            hf->fromNormalizedFloats(asset->normalized);
        } else if (c.source == TerrainSource::Procedural) {
            world::generateNoise(*hf, c.noise);
            if (c.hydraulicErosion) world::erodeHydraulic(*hf, c.hydraulic);
            if (c.thermalErosion) world::erodeThermal(*hf, c.thermal);
        }
        if (!c.layers.empty() || !c.splatRules.empty()) {
            u32 layerCount = static_cast<u32>(c.layers.size());
            for (const auto& rule : c.splatRules) layerCount = std::max(layerCount, rule.layer + 1);
            layerCount = std::clamp(layerCount, 1u, world::kMaxSplatLayers);
            const u32 res = c.splatResolution >= 2 ? c.splatResolution : hf->resolution();
            splat = std::make_shared<world::SplatMap>(res, layerCount, d.origin, d.worldSize);
            splat->fill(0);
            if (!c.splatRules.empty()) world::autoPaint(*splat, *hf, c.splatRules);
        }
    }

    if (hf) {
        st.hf = std::move(hf);
        st.splat = std::move(splat);
        world::TerrainLodSettings lod = c.lod;
        lod.leafNodeSize = std::max(2u, lod.leafNodeSize & ~1u);
        lod.lodCount = std::clamp(lod.lodCount, 1u, 12u);
        st.quadtree = std::make_shared<world::TerrainQuadtree>(*st.hf, lod);
        st.grid = gridMesh(lod.leafNodeSize, st.gridVersion);
        ++st.version;
        ++st.splatVersion;
        st.fullUpload = true;
        st.splatFullUpload = st.splat != nullptr;
        st.dirtyRect = st.hf->fullRect();
        st.splatDirtyRect = st.splat ? st.splat->fullRect() : world::IRect{};

        // Streamed vegetation instances of external tiles, prepared for the renderer once.
        st.externalVegGpu.reset();
        st.externalVegCells.reset();
        if (st.hasExternal && !st.external.vegetation.empty()) {
            world::VegetationChunk chunk;
            chunk.origin = st.hf->desc().origin;
            chunk.size = st.hf->desc().worldSize;
            chunk.instances = st.external.vegetation;
            std::vector<world::VegetationLayer> layers = st.external.vegetationLayers;
            u16 maxLayer = 0;
            for (const auto& inst : chunk.instances) maxLayer = std::max(maxLayer, inst.layer);
            if (layers.size() <= maxLayer) layers.resize(usize(maxLayer) + 1);
            chunk.buildCells(layers, 32.f);
            auto gpu = std::make_shared<std::vector<world::VegetationInstanceGpu>>();
            gpu->reserve(chunk.instances.size());
            for (const auto& inst : chunk.instances) gpu->push_back(world::toGpu(inst, layers[inst.layer]));
            st.externalVegGpu = std::move(gpu);
            st.externalVegCells = std::make_shared<const std::vector<world::VegetationCell>>(chunk.cells);
            st.externalVegVersion = ++vegetationCounter;
        }
    }
    if (!st.hf) return;
    auto& mc = world->registry().get<TerrainComponent>(e.handle());
    mc.builtResolution = st.hf->resolution();
    mc.minHeight = st.hf->minHeight();
    mc.maxHeight = st.hf->maxHeight();
}

void WorldRuntime::Impl::destroyTerrainBodies(TerrainState& st) {
    for (auto& [key, body] : st.bodies) destroyBody(body);
    st.bodies.clear();
    st.bodiesBuilt = false;
}

void WorldRuntime::Impl::buildTerrainBodies(Entity e, TerrainState& st) {
    OX_PROFILE_ZONE();
    destroyTerrainBodies(st);
    st.bodiesBuilt = true;
    const auto& c = e.get<TerrainComponent>();
    if (!c.collision || !st.hf) return;
    const u32 quads = std::max(1u, c.physicsTileQuads);
    const u64 id = toRuntimeId(e);
    for (const world::PhysicsHeightfieldTile& tile : world::buildPhysicsTiles(*st.hf, quads)) {
        physics::BodyHandle b = createStaticBody(world::toShapeDesc(tile), glm::vec3(0.f), glm::quat(1.f, 0.f, 0.f, 0.f), id,
                                                 c.friction, c.restitution, false);
        if (b) st.bodies[{tile.tileX, tile.tileZ}] = b;
    }
    if (!st.bodies.empty()) physicsRt->physicsWorld().optimizeBroadPhase();
}

void WorldRuntime::Impl::rebuildTerrainTiles(Entity e, TerrainState& st, const world::IRect& dirty) {
    const auto* c = e.tryGet<TerrainComponent>();
    if (!c || !c->collision || !st.hf || !physicsEnabled()) return;
    const u32 quads = std::max(1u, c->physicsTileQuads);
    const u64 id = toRuntimeId(e);
    for (const glm::ivec2 t : world::physicsTilesOverlapping(dirty, quads, st.hf->resolution())) {
        auto it = st.bodies.find({t.x, t.y});
        if (it != st.bodies.end()) destroyBody(it->second);
        const world::PhysicsHeightfieldTile tile = world::buildPhysicsTile(*st.hf, t.x, t.y, quads);
        physics::BodyHandle b = createStaticBody(world::toShapeDesc(tile), glm::vec3(0.f), glm::quat(1.f, 0.f, 0.f, 0.f), id,
                                                 c->friction, c->restitution, false);
        if (b) st.bodies[{t.x, t.y}] = b;
        else st.bodies.erase({t.x, t.y});
    }
}

void WorldRuntime::Impl::onAssetChanged(const GameplayAssetChange& change) {
    if (change.kind != GameplayAssetKind::Heightmap || !world) return;
    for (auto [e, c] : world->registry().view<TerrainComponent>().each()) {
        if (c.source == TerrainSource::Heightmap && c.heightmap == change.id) terrains[e].dirty = true;
    }
}

void WorldRuntime::updateTerrains() {
    OX_PROFILE_ZONE();
    if (!m->world) return;
    entt::registry& r = m->world->registry();
    std::vector<entt::entity> list;
    for (auto e : r.view<TerrainComponent>()) list.push_back(e);
    std::sort(list.begin(), list.end());
    for (auto e : list) {
        const Entity ent = m->world->wrap(e);
        TerrainState& st = m->terrains[e];
        const auto& c = r.get<TerrainComponent>(e);
        if (!st.dirty && st.hf && c.source != TerrainSource::External) {
            const glm::vec3 p = ent.worldPosition();
            if (glm::any(glm::greaterThan(glm::abs(p - st.builtPosition), glm::vec3(1e-4f)))) st.dirty = true;
        }
        if (st.dirty) {
            const u64 before = st.version;
            m->buildTerrain(ent, st);
            if (st.version != before && st.bodiesBuilt) st.bodiesBuilt = false; // rebuild colliders
        }
        // Colliders follow every rebuild (toggling `collision` is a component change -> rebuild).
        if (m->physicsEnabled() && !st.bodiesBuilt && st.hf) m->buildTerrainBodies(ent, st);
    }
}

} // namespace ox::gameplay
