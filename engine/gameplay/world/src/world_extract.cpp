#include "world_runtime_impl.hpp"

#include <oxwald/core/profile.hpp>
#include <oxwald/gameplay/common.hpp>

#include <algorithm>
#include <cmath>

namespace ox::gameplay {

namespace {

VegetationPrototypeInfo prototypeInfo(const world::VegetationLayer& l, u16 index) {
    VegetationPrototypeInfo p;
    p.name = l.name;
    p.layer = index;
    p.prototype = l.prototype;
    p.kind = l.kind;
    p.boundingRadius = l.boundingRadius;
    p.lod = l.lod;
    p.castsShadow = l.kind == world::VegetationKind::Tree;
    return p;
}

} // namespace

void WorldRuntime::extract(WorldRenderData& out, u64 frame) {
    OX_PROFILE_ZONE();
    out.clear();
    out.frame = frame;
    ++out.version;
    out.time = m->time;
    if (!m->world) return;
    World& w = *m->world;
    entt::registry& r = w.registry();

    // ---- primary camera ----
    world::Frustum frustum = world::Frustum::infinite();
    if (Entity cam = m->primaryCamera(); cam.valid()) {
        Transform t = cam.worldTransform();
        t.rotation = glm::normalize(t.rotation);
        t.scale = glm::vec3(1.f);
        out.hasCamera = true;
        out.cameraEntity = toRuntimeId(cam);
        out.cameraPosition = t.position;
        out.view = glm::inverse(t.toMatrix());
        out.projection = cam.get<CameraComponent>().projectionMatrix(out.aspect > 0.f ? out.aspect : 16.f / 9.f);
        frustum = world::Frustum::fromViewProjection(out.projection * out.view);
    }

    // ---- terrains ----
    std::vector<entt::entity> terrainList;
    for (const auto& [e, st] : m->terrains) {
        if (st.hf && w.valid(e) && r.all_of<TerrainComponent>(e)) terrainList.push_back(e);
    }
    std::sort(terrainList.begin(), terrainList.end());
    world::TerrainSelection selection;
    for (auto e : terrainList) {
        TerrainState& st = m->terrains[e];
        const auto& c = r.get<TerrainComponent>(e);
        TerrainRenderItem item;
        item.entity = toRuntimeId(e);
        item.heightfield = st.hf;
        item.heightfieldVersion = st.version;
        item.dirtyRect = st.dirtyRect;
        item.fullUpload = st.fullUpload;
        item.splat = st.splat;
        item.splatVersion = st.splatVersion;
        item.splatDirtyRect = st.splatDirtyRect;
        item.splatFullUpload = st.splatFullUpload;
        item.layerMaterials = c.layers;
        item.gridMesh = st.grid;
        item.gridMeshVersion = st.gridVersion;
        item.quadtree = st.quadtree;
        if (st.quadtree) {
            item.lodRanges = st.quadtree->ranges();
            const u32 lods = static_cast<u32>(item.lodRanges.range.size());
            item.skirtDepth.reserve(lods);
            for (u32 l = 0; l < lods; ++l) item.skirtDepth.push_back(st.quadtree->skirtDepth(l));
            if (out.hasCamera) {
                selection.clear();
                st.quadtree->select({.cameraPosition = out.cameraPosition, .frustum = frustum}, selection);
                item.patches.reserve(selection.patches.size());
                for (const world::TerrainPatch& p : selection.patches) item.patches.push_back(world::toGpu(p));
            }
        }
        const world::HeightfieldDesc& d = st.hf->desc();
        item.bounds.min = {d.origin.x, st.hf->minHeight(), d.origin.y};
        item.bounds.max = {d.origin.x + d.worldSize, st.hf->maxHeight(), d.origin.y + d.worldSize};
        out.terrains.push_back(std::move(item));
        st.dirtyRect = {};
        st.fullUpload = false;
        st.splatDirtyRect = {};
        st.splatFullUpload = false;

        if (st.externalVegGpu && !st.externalVegGpu->empty()) {
            VegetationRenderItem veg;
            veg.entity = toRuntimeId(e);
            for (usize i = 0; i < st.external.vegetationLayers.size(); ++i) {
                veg.layers.push_back(prototypeInfo(st.external.vegetationLayers[i], static_cast<u16>(i)));
            }
            VegetationBatch batch;
            batch.origin = d.origin;
            batch.size = d.worldSize;
            batch.version = st.externalVegVersion;
            batch.instances = st.externalVegGpu;
            batch.cells = st.externalVegCells;
            veg.batches.push_back(std::move(batch));
            out.vegetation.push_back(std::move(veg));
        }
    }

    // ---- vegetation ----
    std::vector<entt::entity> vegList;
    for (const auto& [e, st] : m->vegetation) {
        if (st.scatterer && w.valid(e) && !st.chunks.empty()) vegList.push_back(e);
    }
    std::sort(vegList.begin(), vegList.end());
    for (auto e : vegList) {
        const VegetationState& st = m->vegetation[e];
        VegetationRenderItem item;
        item.entity = toRuntimeId(e);
        const auto& layers = st.scatterer->layers();
        for (usize i = 0; i < layers.size(); ++i) item.layers.push_back(prototypeInfo(layers[i], static_cast<u16>(i)));
        for (const auto& [coord, chunk] : st.chunks) {
            if (!chunk.gpu || chunk.gpu->empty()) continue;
            VegetationBatch batch;
            batch.origin = chunk.chunk.origin;
            batch.size = chunk.chunk.size;
            batch.version = chunk.version;
            batch.instances = chunk.gpu;
            batch.cells = chunk.cells;
            item.batches.push_back(std::move(batch));
        }
        out.vegetation.push_back(std::move(item));
    }

    // ---- sky ----
    if (const entt::entity skyEntity = m->firstWith<SkyComponent>(); skyEntity != entt::null) {
        const auto& sc = r.get<SkyComponent>(skyEntity);
        SkyRenderData& s = out.sky;
        s.valid = true;
        s.skyIntensity = sc.skyIntensity;
        s.sunDisc = sc.sunDisc;
        s.sunDiscIntensity = sc.sunDiscIntensity;
        s.sunAngularDiameterDeg = sc.sunAngularDiameterDeg;
        s.moon = sc.moon;
        s.moonIntensity = sc.moonIntensity;
        s.moonAngularDiameterDeg = sc.moonAngularDiameterDeg;
        s.stars = sc.stars;
        if (const world::SkyState* st = skyState()) {
            s.preetham = st->preetham.toGpu();
            s.sunDirection = st->sunDirection;
            s.moonDirection = st->moonDirection;
            s.moonPhase = st->moonPhase;
            s.starsRotation = st->starsRotation;
            s.atmosphere = st->atmosphere;
            s.sunLight = st->sunLight;
            s.moonLight = st->moonLight;
            s.mainLightDirection = st->mainLightDirection;
            s.mainLightColor = st->mainLightColor;
            s.mainLightIlluminance = st->mainLightIlluminance;
            s.isDay = st->isDay;
            s.localHours = st->local.hours;
        } else {
            const glm::vec3 dir = glm::length(sc.sunDirection) > 1e-6f ? glm::normalize(sc.sunDirection) : glm::vec3(0.f, 1.f, 0.f);
            const f64 elevation = glm::degrees(std::asin(std::clamp(dir.y, -1.f, 1.f)));
            s.preetham = world::PreethamSky::compute(dir, sc.turbidity).toGpu();
            s.sunDirection = dir;
            s.sunLight = world::sunLight(elevation, sc.turbidity);
            s.atmosphere = world::AtmosphereCurves::defaults().evaluate(static_cast<f32>(elevation), 12.f);
            s.mainLightDirection = dir;
            s.mainLightColor = s.sunLight.color;
            s.mainLightIlluminance = s.sunLight.illuminance;
            s.isDay = elevation > -0.833;
        }
        s.starsIntensity = sc.starsIntensity * s.atmosphere.starsIntensity;
    }

    // ---- water ----
    std::vector<entt::entity> waterList;
    for (const auto& [e, st] : m->waters) {
        if (!st.dirty && w.valid(e) && r.all_of<WaterComponent>(e)) waterList.push_back(e);
    }
    std::sort(waterList.begin(), waterList.end());
    const f32 t = static_cast<f32>(m->time);
    for (auto e : waterList) {
        const WaterState& st = m->waters[e];
        WaterRenderItem item;
        item.entity = toRuntimeId(e);
        item.params = st.waves.toGpu(t);
        item.transform = w.wrap(e).worldMatrix();
        item.size = st.size;
        out.water.push_back(item);
    }

    // ---- wind / weather ----
    if (auto it = m->winds.find(m->globalWind); it != m->winds.end() && w.valid(m->globalWind)) {
        out.hasWind = true;
        out.wind = it->second.field.toGpu(t);
    }
    if (const world::WeatherState* ws = weather()) {
        out.hasWeather = true;
        out.weather = *ws;
    }
}

} // namespace ox::gameplay
