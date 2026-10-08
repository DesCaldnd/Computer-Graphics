#pragma once

#include <oxwald/ai/navmesh.hpp>

#include <Recast.h>

#include <memory>
#include <vector>

namespace ox::ai::detail {

struct RcDeleter {
    void operator()(rcHeightfield* p) const { rcFreeHeightField(p); }
    void operator()(rcCompactHeightfield* p) const { rcFreeCompactHeightfield(p); }
    void operator()(rcContourSet* p) const { rcFreeContourSet(p); }
    void operator()(rcPolyMesh* p) const { rcFreePolyMesh(p); }
    void operator()(rcPolyMeshDetail* p) const { rcFreePolyMeshDetail(p); }
    void operator()(rcHeightfieldLayerSet* p) const { rcFreeHeightfieldLayerSet(p); }
};
template <class T>
using RcPtr = std::unique_ptr<T, RcDeleter>;

// Base Recast config from engine settings (bounds/size left to the caller).
rcConfig makeConfig(const NavMeshBuildSettings& s);

// Rasterises the triangles overlapping cfg bounds and produces an eroded, area-marked compact heightfield.
RcPtr<rcCompactHeightfield> buildCompactHeightfield(rcContext& ctx, const rcConfig& cfg, const NavMeshInput& input,
                                                    const NavMeshBuildSettings& s);

// Builds Detour tile data (dtCreateNavMeshData output, owned by caller via dtFree) for the given config.
// tx/ty identify the tile (0,0 for solo). Returns false when the area contains no walkable polygons.
bool buildTileData(rcContext& ctx, const rcConfig& cfg, const NavMeshInput& input, const NavMeshBuildSettings& s,
                   i32 tx, i32 ty, unsigned char** outData, i32* outSize);

// Tile config (with border) for tile (tx, ty) of a grid starting at origin.
rcConfig makeTileConfig(const NavMeshBuildSettings& s, const glm::vec3& bmin, const glm::vec3& bmax,
                        i32 tx, i32 ty);

u32 nextPow2(u32 v);
u32 ilog2(u32 v);

} // namespace ox::ai::detail
