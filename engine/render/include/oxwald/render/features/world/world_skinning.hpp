#pragma once

// World-skinning feature area:
//   * "WorldGeometry": CDLOD terrain (heightmap R16/R32F with dirty-rect uploads, normals from the heightmap, splat
//     blending of up to 8 PBR layers with triplanar / height blend / macro variation / tiling breakup, holes, CSM
//     shadows) and vegetation (GPU frustum/distance/LOD culling per cell into indirect draws, dithered LOD cross-fade,
//     octahedral impostors baked at load, wind, two-sided translucent foliage, grass bending around interactors).
//     Terrain and vegetation are merged into the core frame: their depth/normals/velocity are drawn right after the
//     depth prepass (HiZ is rebuilt), their sun shadows into the cascades (ShadowMask is rebuilt) and they are shaded
//     before the sky.
//   * "WorldSky": Preetham sky with sun disc + limb darkening, moon with phase, procedural rotating star field,
//     night sky, aerial perspective (skipped when volumetric fog is active), sky cube for the IBL (refreshed when the
//     sun/moon moved by more than r.Sky.IBLUpdateDegrees). Replaces the core "Sky" feature (same exclusive group).
//   * "Skinning": compute skinning (linear or dual quaternion) of SkinnedMesh palettes into per-instance vertex
//     buffers (current + previous frame for motion vectors) consumed by every core draw through oxFetchVertex.
//
// Data comes from snapshot extensions: WorldSnapshot (terrain / vegetation / sky / wind, filled from gameplay's
// WorldRenderData by the runtime bridge or by hand in tools/tests) and SkinningSnapshot. See
// docs/dev/modules/render.md, section "World: terrain, vegetation, sky, skinning".

#include <oxwald/render/components/world.hpp>
#include <oxwald/render/render_feature.hpp>
#include <oxwald/render/snapshot.hpp>
#include <oxwald/rhi/handles.hpp>

#include <memory>
#include <unordered_map>
#include <vector>

#if OX_RENDER_HAS_WORLD
#include <oxwald/world/heightfield.hpp>
#include <oxwald/world/sky.hpp>
#include <oxwald/world/splat_map.hpp>
#include <oxwald/world/terrain_lod.hpp>
#include <oxwald/world/time_of_day.hpp>
#include <oxwald/world/vegetation.hpp>
#include <oxwald/world/weather.hpp>
#endif

namespace ox::render {

class Renderer;

// Called from registerBuiltinFeatures(): WorldGeometry, WorldSky, Skinning + the area's cvars, types and extract hooks.
void registerWorldSkinningFeatures(FeatureRegistry& features);

// --- skinning ----------------------------------------------------------------------------------------------------

enum class GpuSkinningMethod : u8 { Linear, DualQuaternion };

// Per skinned entity options (encodeEntityId → method). Entities without an entry use linear blend skinning.
struct SkinningSnapshot : ISnapshotExtension {
    std::unordered_map<u32, GpuSkinningMethod> methods;
    void clear() override { methods.clear(); }
};

// Compute skinning output of one skinned entity (all its submeshes share the vertices).
struct SkinnedOutput {
    u32 entityId = 0;      // encodeEntityId
    u32 meshInfoIndex = 0; // GpuMeshInfo of submesh 0 (vertexOffset = base vertex of the mesh in the scene arenas)
    u32 vertexCount = 0;
    u64 currentOffset = 0;  // byte offset of this frame's vertices in SkinnedOutputs::buffer
    u64 previousOffset = 0; // previous frame (== currentOffset on the first frame)
};

// Hook for the ray tracing team (BLAS refit of skinned meshes) and other consumers. Vertices are GpuSkinnedVertex
// (model space, 40 bytes: position at offset 0 → BLAS vertexStride 40, R32G32B32_SFLOAT). The buffer is also
// published to the frame graph as resource "SkinnedVertices" (kSkinnedVerticesResource) after the skinning pass
// (InjectionPoint::PreDepth); declare a read on it (e.g. AccelStructBuildRead) to get the barrier, e.g.
//   if (auto* s = dynamic_cast<const ISkinnedOutputs*>(renderer.features().find("Skinning")))
//       for (const SkinnedOutput& o : s->skinnedOutputs().items) refit(blasOf(o.entityId), buffer, o.currentOffset);
inline constexpr std::string_view kSkinnedVerticesResource = "SkinnedVertices";
struct SkinnedOutputs {
    rhi::BufferHandle buffer;
    u32 stride = 40;
    std::vector<SkinnedOutput> items; // this frame
};
class ISkinnedOutputs {
public:
    virtual ~ISkinnedOutputs() = default;
    [[nodiscard]] virtual const SkinnedOutputs& skinnedOutputs() const = 0;
};

#if OX_RENDER_HAS_WORLD

// --- world snapshot ------------------------------------------------------------------------------------------------

struct TerrainSnapshot {
    u32 entityId = 0; // encodeEntityId (picking), 0 = none
    // Immutable after publication (the gameplay bridge copies the runtime heightfield when its version changes).
    std::shared_ptr<const world::Heightfield> heightfield;
    u64 heightfieldVersion = 0;
    // Samples changed since `dirtySinceVersion` (half-open). When the renderer's uploaded version differs from
    // dirtySinceVersion (dropped snapshot, first sight) or fullUpload is set, everything is uploaded again.
    world::IRect dirtyRect{};
    u64 dirtySinceVersion = 0;
    bool fullUpload = false;
    std::shared_ptr<const world::SplatMap> splat; // null = no layers (procedural slope/height colouring)
    u64 splatVersion = 0;
    world::IRect splatDirtyRect{};
    u64 splatDirtySinceVersion = 0;
    bool splatFullUpload = false;
    std::vector<Uuid> layerMaterials; // material per splat layer (≤ 8)
    world::TerrainLodSettings lod{};   // CDLOD settings; selection runs in the renderer (r.Terrain.LODScale)
    TerrainRenderComponent settings{};
};

struct VegetationLayerSnapshot {
    u16 prototype = 0;
    world::VegetationKind kind = world::VegetationKind::Grass;
    f32 boundingRadius = 1.0f;
    world::VegetationLodSettings lod{};
    bool castsShadow = false;
};

struct VegetationBatchSnapshot {
    u64 key = 0;     // stable identity (entity + chunk); GPU instances are re-uploaded when `version` changes
    u64 version = 0;
    std::shared_ptr<const std::vector<world::VegetationInstanceGpu>> instances; // immutable
    std::shared_ptr<const std::vector<world::VegetationCell>> cells;            // ranges into instances, world AABBs
};

struct VegetationSnapshot {
    u32 entityId = 0;
    std::vector<VegetationLayerSnapshot> layers; // index = instance layer (cell.layer)
    std::vector<VegetationBatchSnapshot> batches;
};

struct WorldSkySnapshot {
    bool valid = false;
    world::PreethamSky::Gpu preetham{};
    glm::vec3 sunDirection{0.0f, 1.0f, 0.0f}; // towards the sun
    glm::vec3 moonDirection{0.0f, -1.0f, 0.0f};
    world::MoonPhase moonPhase{};
    glm::quat starsRotation{1.0f, 0.0f, 0.0f, 0.0f}; // celestial → world
    world::AtmosphereParams atmosphere{};
    world::CelestialLight sunLight{};
    world::CelestialLight moonLight{};
    f32 skyIntensity = 1.0f;
    bool sunDisc = true;
    f32 sunDiscIntensity = 1.0f;
    f32 sunAngularDiameterDeg = 0.53f;
    bool moon = true;
    f32 moonIntensity = 1.0f;
    f32 moonAngularDiameterDeg = 0.52f;
    bool stars = true;
    f32 starsIntensity = 1.0f; // final multiplier (component × atmosphere curve)
};

struct WorldSnapshot : ISnapshotExtension {
    f64 time = 0.0; // game time (wind, star twinkle)
    std::vector<TerrainSnapshot> terrains;
    std::vector<VegetationSnapshot> vegetation;
    std::vector<VegetationPrototypeDesc> prototypes; // VegetationPrototypesComponent(s), filled by the area's hook
    std::unordered_map<u32, TerrainRenderComponent> terrainSettings; // by terrain entity id (render hook)
    WorldSkySnapshot sky;
    bool hasWind = false;
    world::WindGpu wind{};
    std::vector<glm::vec4> interactors; // grass benders: xyz world position, w radius (characters, ≤ 8 used)

    void clear() override;
};

// Quantised sun/moon/intensity key of a sky (changes when the sun or moon moved by more than `degrees`).
[[nodiscard]] u64 worldSkyIblKey(const WorldSkySnapshot& sky, f32 degrees);
// Call after filling the WorldSnapshot: applies TerrainRenderComponent settings, sets SnapshotEnvironment::iblKey
// (throttled IBL refresh) and adds a default environment when a sky exists without an EnvironmentComponent.
void finalizeWorldSnapshot(RenderSnapshot& snapshot);

#endif // OX_RENDER_HAS_WORLD

} // namespace ox::render
