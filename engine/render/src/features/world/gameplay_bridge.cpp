// Gameplay → render bridge of the world-skinning area (compiled when render links gameplay with its world
// integration): an ExtractHookEx that reads the WorldRenderData service (terrain / vegetation / sky / wind, filled by
// the Gameplay.World.Extract system) and SkinnedMeshComponent palettes from the World.
//
// WorldRenderData shares runtime-owned CPU data that the game thread may modify after extract (brush edits), so
// heightfields and splat maps are copied here (once per version) into immutable snapshots.
#include "world_internal.hpp"

#include <iterator>

#if OX_RENDER_HAS_GAMEPLAY && OX_RENDER_HAS_WORLD && OX_GAMEPLAY_HAS_WORLD

#include <oxwald/core/hash.hpp>
#include <oxwald/core/profile.hpp>
#include <oxwald/core/services.hpp>
#include <oxwald/gameplay/animation.hpp>
#include <oxwald/gameplay/physics.hpp>
#include <oxwald/gameplay/world/render_data.hpp>
#include <oxwald/render/features/volumetrics/volumetrics.hpp>
#include <oxwald/scene/world.hpp>

#include <bit>
#include <mutex>
#include <unordered_map>

namespace ox::render::worldfx {

namespace {

u32 pickId(u64 runtimeId) { return runtimeId ? encodeEntityId(u32(runtimeId)) : 0u; }

struct TerrainCache {
    u64 version = ~0ull;
    std::shared_ptr<const world::Heightfield> heightfield;
    u64 splatVersion = ~0ull;
    std::shared_ptr<const world::SplatMap> splat;
};

struct PaletteCache {
    std::vector<glm::mat4> current;
    u64 version = ~0ull;
    std::vector<glm::mat4> previous;
    u64 lastFrame = 0;
};

struct BridgeState {
    std::mutex mutex; // extract may run on different threads over time (never concurrently)
    std::unordered_map<u64, TerrainCache> terrains;
    std::unordered_map<u32, PaletteCache> palettes;
    u64 frame = 0;
};

BridgeState& state() {
    static BridgeState s;
    return s;
}

void extractWorld(const gameplay::WorldRenderData& wrd, RenderSnapshot& out, BridgeState& st) {
    WorldSnapshot& ws = out.extension<WorldSnapshot>();
    ws.time = wrd.time;
    for (const gameplay::TerrainRenderItem& t : wrd.terrains) {
        if (!t.heightfield) continue;
        TerrainCache& c = st.terrains[t.entity];
        TerrainSnapshot s;
        s.entityId = pickId(t.entity);
        s.dirtySinceVersion = c.version;
        if (c.version != t.heightfieldVersion || !c.heightfield) {
            c.heightfield = std::make_shared<world::Heightfield>(*t.heightfield);
            c.version = t.heightfieldVersion;
        }
        s.heightfield = c.heightfield;
        s.heightfieldVersion = t.heightfieldVersion;
        s.dirtyRect = t.dirtyRect;
        s.fullUpload = t.fullUpload;
        if (t.splat) {
            s.splatDirtySinceVersion = c.splatVersion;
            if (c.splatVersion != t.splatVersion || !c.splat) {
                c.splat = std::make_shared<world::SplatMap>(*t.splat);
                c.splatVersion = t.splatVersion;
            }
            s.splat = c.splat;
            s.splatVersion = t.splatVersion;
            s.splatDirtyRect = t.splatDirtyRect;
            s.splatFullUpload = t.splatFullUpload;
        }
        s.layerMaterials = t.layerMaterials;
        if (t.quadtree) s.lod = t.quadtree->settings();
        ws.terrains.push_back(std::move(s));
    }
    for (const gameplay::VegetationRenderItem& v : wrd.vegetation) {
        VegetationSnapshot vs;
        vs.entityId = pickId(v.entity);
        for (const gameplay::VegetationPrototypeInfo& l : v.layers) {
            vs.layers.push_back({l.prototype, l.kind, l.boundingRadius, l.lod, l.castsShadow});
        }
        for (const gameplay::VegetationBatch& b : v.batches) {
            u64 key = hashCombine(v.entity, std::bit_cast<u32>(b.origin.x));
            key = hashCombine(key, std::bit_cast<u32>(b.origin.y));
            vs.batches.push_back({key, b.version, b.instances, b.cells});
        }
        ws.vegetation.push_back(std::move(vs));
    }
    if (wrd.sky.valid) {
        const gameplay::SkyRenderData& s = wrd.sky;
        WorldSkySnapshot& k = ws.sky;
        k.valid = true;
        k.preetham = s.preetham;
        k.sunDirection = s.sunDirection;
        k.moonDirection = s.moonDirection;
        k.moonPhase = s.moonPhase;
        k.starsRotation = s.starsRotation;
        k.atmosphere = s.atmosphere;
        k.sunLight = s.sunLight;
        k.moonLight = s.moonLight;
        k.skyIntensity = s.skyIntensity;
        k.sunDisc = s.sunDisc;
        k.sunDiscIntensity = s.sunDiscIntensity;
        k.sunAngularDiameterDeg = s.sunAngularDiameterDeg;
        k.moon = s.moon;
        k.moonIntensity = s.moonIntensity;
        k.moonAngularDiameterDeg = s.moonAngularDiameterDeg;
        k.stars = s.stars;
        k.starsIntensity = s.starsIntensity;
    }
    ws.hasWind = wrd.hasWind;
    ws.wind = wrd.wind;
    if (wrd.hasWind) {
        // Fog volume noise / clouds scroll with the global wind (volumetrics area).
        const glm::vec4 w = wrd.wind.dirSpeedTime;
        volumetrics::setWorldWind(out, glm::vec3(w.x * w.z, 0.0f, w.y * w.z));
    }
}

void extractSkinned(const World& world, RenderSnapshot& out, BridgeState& st) {
    const entt::registry& reg = world.registry();
    SkinningSnapshot* methods = nullptr;
    for (auto [e, sk, wt] : reg.view<const gameplay::SkinnedMeshComponent, const WorldTransformComponent>().each()) {
        if (!sk.visible || !sk.mesh.isValid()) continue;
        if (const auto* a = reg.try_get<ActiveComponent>(e); a && !a->active) continue;
        SnapshotMesh m;
        m.entityId = encodeEntityId(u32(entt::to_integral(e)));
        m.mesh = sk.mesh;
        m.materialOffset = u32(out.materials.size());
        m.materialCount = u32(sk.materials.size());
        out.materials.insert(out.materials.end(), sk.materials.begin(), sk.materials.end());
        m.world = wt.matrix;
        m.prevWorld = wt.previous;
        m.flags = (sk.castShadows ? kMeshCastShadows : 0u) | kMeshReceiveShadows;
        if (sk.gpuSkinning && !sk.palette.empty()) {
            PaletteCache& pc = st.palettes[m.entityId];
            if (pc.version != sk.paletteVersion) {
                pc.previous = pc.current.empty() ? sk.palette : pc.current;
                pc.current = sk.palette;
                pc.version = sk.paletteVersion;
            } else {
                pc.previous = pc.current; // not animated this frame: no skinning motion
            }
            pc.lastFrame = st.frame;
            m.paletteOffset = u32(out.palettes.size());
            m.paletteCount = u32(pc.current.size());
            out.palettes.insert(out.palettes.end(), pc.current.begin(), pc.current.end());
            if (pc.previous.size() == pc.current.size()) {
                m.prevPaletteOffset = u32(out.palettes.size());
                out.palettes.insert(out.palettes.end(), pc.previous.begin(), pc.previous.end());
            }
            if (sk.skinningMethod == anim::SkinningMethod::DualQuaternion) {
                if (!methods) methods = &out.extension<SkinningSnapshot>();
                methods->methods[m.entityId] = GpuSkinningMethod::DualQuaternion;
            }
        }
        out.meshes.push_back(m);
    }
    for (auto it = st.palettes.begin(); it != st.palettes.end();) {
        it = it->second.lastFrame + 120 < st.frame ? st.palettes.erase(it) : std::next(it);
    }
}

void bridgeHook(const World& world, RenderSnapshot& out, const ExtractOptions& options) {
    OX_PROFILE_ZONE_N("render.worldBridge");
    BridgeState& st = state();
    std::lock_guard lock(st.mutex);
    ++st.frame;
    extractSkinned(world, out, st);
    if (options.services) {
        if (const auto* wrd = options.services->tryGet<gameplay::WorldRenderData>()) extractWorld(*wrd, out, st);
    }
    // Grass benders: character controllers.
    const entt::registry& reg = world.registry();
    WorldSnapshot* ws = nullptr;
    for (auto [e, cc, wt] : reg.view<const gameplay::CharacterControllerComponent, const WorldTransformComponent>().each()) {
        if (!ws) ws = &out.extension<WorldSnapshot>();
        if (ws->interactors.size() >= 8) break;
        ws->interactors.push_back(glm::vec4(glm::vec3(wt.matrix[3]), std::max(cc.radius * 2.5f, 0.6f)));
    }
    finalizeWorldSnapshot(out);
}

} // namespace

void registerGameplayBridge() { addExtractHookEx(&bridgeHook); }

} // namespace ox::render::worldfx

#else

namespace ox::render::worldfx {
#if OX_RENDER_HAS_WORLD
namespace {
// Without gameplay, still apply TerrainRenderComponent settings / IBL keys for snapshots filled by tools and tests.
void finalizeHook(const World&, RenderSnapshot& out, const ExtractOptions&) { finalizeWorldSnapshot(out); }
} // namespace
void registerGameplayBridge() { addExtractHookEx(&finalizeHook); }
#else
void registerGameplayBridge() {}
#endif
} // namespace ox::render::worldfx

#endif
