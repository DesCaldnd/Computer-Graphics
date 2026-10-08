#pragma once

// ECS components of the world-skinning area (render-side settings of the open world). Terrain, vegetation scattering,
// sky and time of day themselves are gameplay components (TerrainComponent, VegetationComponent, SkyComponent, ...);
// these add what only the renderer needs. Reflected and registered by registerWorldSkinningTypes() (called from
// render::registerRenderTypes() and registerWorldSkinningFeatures()); extracted by the area's extract hook into the
// WorldSnapshot extension (features/world/world_skinning.hpp).

#include <oxwald/core/math.hpp>
#include <oxwald/core/types.hpp>
#include <oxwald/core/uuid.hpp>

#include <string>
#include <vector>

namespace ox::render {

// How one vegetation prototype (world::VegetationLayer::prototype) is drawn. Prototypes without a description use a
// built-in procedural mesh chosen by the layer kind (tree, grass clump, bush).
//
// Mesh conventions for wind: object space Y up, origin at the base. Vertex colour R = branch flutter weight,
// G = leaf flutter weight, B = phase offset, A = opacity multiplier. Trunk sway grows with (y / height)².
struct VegetationPrototypeDesc {
    u32 prototype = 0;      // matches world::VegetationLayer::prototype
    std::string name;
    std::vector<Uuid> lods; // mesh per LOD 0..2 (missing entries reuse the previous LOD); empty = built-in mesh
    Uuid material;          // overrides every submesh material (nil = the meshes' own materials)
    bool impostor = true;   // octahedral impostor baked at load from the last mesh LOD (trees / large bushes)
    f32 windSway = 1.0f;    // trunk sway multiplier
    f32 windFlutter = 1.0f; // branch / leaf flutter multiplier
    f32 translucency = 0.6f; // leaf back-lighting (two-sided foliage)
    bool castShadows = true; // trees only (world::kVegFlagCastsShadow)
};

// Any entity; all components of the world are merged (later prototype indices override earlier ones).
struct VegetationPrototypesComponent {
    std::vector<VegetationPrototypeDesc> prototypes;
};

// Optional render settings of a terrain (same entity as gameplay's TerrainComponent).
struct TerrainRenderComponent {
    f32 triplanarSlopeDeg = 35.0f; // slopes steeper than this blend in triplanar projection (r.Terrain.Triplanar)
    f32 heightBlend = 0.2f;        // height-based layer blend depth (0 = plain weights)
    f32 macroVariation = 0.35f;    // low-frequency albedo variation (breaks distant repetition)
    f32 tilingBreakup = 0.5f;      // second rotated/scaled sample blended by noise
    f32 layerTileMeters = 4.0f;    // world size of one texture repeat at material uvTiling 1
    bool castShadows = true;
    f32 tessellationHeight = 0.08f; // displacement amplitude (m) of the optional tessellation detail
};

void registerWorldSkinningTypes();

} // namespace ox::render
