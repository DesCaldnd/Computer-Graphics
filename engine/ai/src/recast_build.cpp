#include "recast_build.hpp"

#include <oxwald/core/log.hpp>

#include <DetourAlloc.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ox::ai {

std::array<u16, NavArea::Count> defaultAreaFlags() {
    std::array<u16, NavArea::Count> f{};
    f.fill(NavFlags::Walk);
    f[NavArea::Null] = 0;
    f[NavArea::Water] = NavFlags::Swim;
    f[NavArea::Door] = NavFlags::Walk | NavFlags::Door;
    f[NavArea::Jump] = NavFlags::Jump;
    return f;
}

void NavMeshInput::addQuad(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& d, u8 area) {
    const u32 base = static_cast<u32>(vertices.size());
    triAreas.resize(indices.size() / 3, NavArea::Ground);
    vertices.insert(vertices.end(), {a, b, c, d});
    indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    triAreas.insert(triAreas.end(), {area, area});
}

void NavMeshInput::addBox(const glm::vec3& mn, const glm::vec3& mx, u8 area) {
    const glm::vec3 p[8] = {{mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, {mx.x, mn.y, mx.z}, {mn.x, mn.y, mx.z},
                            {mn.x, mx.y, mn.z}, {mx.x, mx.y, mn.z}, {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z}};
    addQuad(p[4], p[7], p[6], p[5], area); // top (up)
    addQuad(p[0], p[1], p[2], p[3], area); // bottom (down)
    addQuad(p[0], p[4], p[5], p[1], area); // -Z
    addQuad(p[2], p[6], p[7], p[3], area); // +Z
    addQuad(p[3], p[7], p[4], p[0], area); // -X
    addQuad(p[1], p[5], p[6], p[2], area); // +X
}

void NavMeshInput::append(const NavMeshInput& o) {
    const u32 base = static_cast<u32>(vertices.size());
    triAreas.resize(indices.size() / 3, NavArea::Ground);
    vertices.insert(vertices.end(), o.vertices.begin(), o.vertices.end());
    for (u32 i : o.indices) {
        indices.push_back(base + i);
    }
    for (usize t = 0; t < o.indices.size() / 3; ++t) {
        triAreas.push_back(t < o.triAreas.size() ? o.triAreas[t] : NavArea::Ground);
    }
    offMeshLinks.insert(offMeshLinks.end(), o.offMeshLinks.begin(), o.offMeshLinks.end());
    volumes.insert(volumes.end(), o.volumes.begin(), o.volumes.end());
}

std::pair<glm::vec3, glm::vec3> NavMeshInput::bounds() const {
    if (vertices.empty()) {
        return {glm::vec3(0.f), glm::vec3(0.f)};
    }
    glm::vec3 mn = vertices.front(), mx = vertices.front();
    for (const auto& v : vertices) {
        mn = glm::min(mn, v);
        mx = glm::max(mx, v);
    }
    return {mn, mx};
}

namespace detail {

u32 nextPow2(u32 v) {
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

u32 ilog2(u32 v) {
    u32 r = 0;
    while (v >>= 1) {
        ++r;
    }
    return r;
}

rcConfig makeConfig(const NavMeshBuildSettings& s) {
    rcConfig cfg{};
    cfg.cs = s.cellSize;
    cfg.ch = s.cellHeight;
    cfg.walkableSlopeAngle = s.agentMaxSlope;
    cfg.walkableHeight = static_cast<int>(std::ceil(s.agentHeight / s.cellHeight));
    cfg.walkableClimb = static_cast<int>(std::floor(s.agentMaxClimb / s.cellHeight));
    cfg.walkableRadius = static_cast<int>(std::ceil(s.agentRadius / s.cellSize));
    cfg.maxEdgeLen = static_cast<int>(s.edgeMaxLen / s.cellSize);
    cfg.maxSimplificationError = s.edgeMaxError;
    cfg.minRegionArea = s.regionMinSize * s.regionMinSize;
    cfg.mergeRegionArea = s.regionMergeSize * s.regionMergeSize;
    cfg.maxVertsPerPoly = std::clamp(s.vertsPerPoly, 3, DT_VERTS_PER_POLYGON);
    cfg.detailSampleDist = s.detailSampleDist < 0.9f ? 0.f : s.cellSize * s.detailSampleDist;
    cfg.detailSampleMaxError = s.cellHeight * s.detailSampleMaxError;
    return cfg;
}

rcConfig makeTileConfig(const NavMeshBuildSettings& s, const glm::vec3& bmin, const glm::vec3& bmax, i32 tx, i32 ty) {
    rcConfig cfg = makeConfig(s);
    const f32 tcs = static_cast<f32>(s.tileSize) * s.cellSize;
    cfg.tileSize = s.tileSize;
    cfg.borderSize = cfg.walkableRadius + 3;
    cfg.width = cfg.tileSize + cfg.borderSize * 2;
    cfg.height = cfg.tileSize + cfg.borderSize * 2;
    cfg.bmin[0] = bmin.x + static_cast<f32>(tx) * tcs - static_cast<f32>(cfg.borderSize) * cfg.cs;
    cfg.bmin[1] = bmin.y;
    cfg.bmin[2] = bmin.z + static_cast<f32>(ty) * tcs - static_cast<f32>(cfg.borderSize) * cfg.cs;
    cfg.bmax[0] = bmin.x + static_cast<f32>(tx + 1) * tcs + static_cast<f32>(cfg.borderSize) * cfg.cs;
    cfg.bmax[1] = bmax.y;
    cfg.bmax[2] = bmin.z + static_cast<f32>(ty + 1) * tcs + static_cast<f32>(cfg.borderSize) * cfg.cs;
    return cfg;
}

RcPtr<rcCompactHeightfield> buildCompactHeightfield(rcContext& ctx, const rcConfig& cfg, const NavMeshInput& input,
                                                    const NavMeshBuildSettings& s) {
    RcPtr<rcHeightfield> hf(rcAllocHeightfield());
    if (!hf || !rcCreateHeightfield(&ctx, *hf, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch)) {
        OX_LOG_ERROR("ai", "navmesh: could not create heightfield");
        return nullptr;
    }

    // Triangles overlapping the build area (XZ) — tiles only rasterise their neighbourhood.
    const usize triCount = input.indices.size() / 3;
    std::vector<int> tris;
    std::vector<u8> userAreas;
    tris.reserve(input.indices.size());
    for (usize t = 0; t < triCount; ++t) {
        const glm::vec3& a = input.vertices[input.indices[t * 3]];
        const glm::vec3& b = input.vertices[input.indices[t * 3 + 1]];
        const glm::vec3& c = input.vertices[input.indices[t * 3 + 2]];
        const glm::vec3 mn = glm::min(a, glm::min(b, c)), mx = glm::max(a, glm::max(b, c));
        if (mx.x < cfg.bmin[0] || mn.x > cfg.bmax[0] || mx.z < cfg.bmin[2] || mn.z > cfg.bmax[2]) {
            continue;
        }
        for (int k = 0; k < 3; ++k) {
            tris.push_back(static_cast<int>(input.indices[t * 3 + k]));
        }
        userAreas.push_back(t < input.triAreas.size() ? input.triAreas[t] : NavArea::Ground);
    }
    const int nt = static_cast<int>(userAreas.size());
    const float* verts = input.vertices.empty() ? nullptr : &input.vertices[0].x;
    const int nv = static_cast<int>(input.vertices.size());
    std::vector<u8> areas(static_cast<usize>(nt), 0);
    if (nt > 0) {
        rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, verts, nv, tris.data(), nt, areas.data());
        for (int i = 0; i < nt; ++i) {
            areas[i] = areas[i] != RC_NULL_AREA ? userAreas[i] : RC_NULL_AREA;
        }
        if (!rcRasterizeTriangles(&ctx, verts, nv, tris.data(), areas.data(), nt, *hf, cfg.walkableClimb)) {
            OX_LOG_ERROR("ai", "navmesh: rasterisation failed");
            return nullptr;
        }
    }
    if (s.filterLowHangingObstacles) {
        rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *hf);
    }
    if (s.filterLedgeSpans) {
        rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf);
    }
    if (s.filterWalkableLowHeightSpans) {
        rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *hf);
    }

    RcPtr<rcCompactHeightfield> chf(rcAllocCompactHeightfield());
    if (!chf || !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf, *chf)) {
        OX_LOG_ERROR("ai", "navmesh: compact heightfield failed");
        return nullptr;
    }
    if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf)) {
        OX_LOG_ERROR("ai", "navmesh: erosion failed");
        return nullptr;
    }
    for (const NavConvexVolume& vol : input.volumes) {
        if (vol.points.size() < 3) {
            continue;
        }
        std::vector<float> pts;
        for (const auto& p : vol.points) {
            pts.insert(pts.end(), {p.x, p.y, p.z});
        }
        rcMarkConvexPolyArea(&ctx, pts.data(), static_cast<int>(vol.points.size()), vol.minY, vol.maxY, vol.area, *chf);
    }
    return chf;
}

bool buildTileData(rcContext& ctx, const rcConfig& cfgIn, const NavMeshInput& input, const NavMeshBuildSettings& s,
                   i32 tx, i32 ty, unsigned char** outData, i32* outSize) {
    rcConfig cfg = cfgIn;
    *outData = nullptr;
    *outSize = 0;
    RcPtr<rcCompactHeightfield> chf = buildCompactHeightfield(ctx, cfg, input, s);
    if (!chf) {
        return false;
    }
    if (chf->spanCount == 0) {
        return false;
    }

    switch (s.partition) {
    case NavPartition::Watershed:
        if (!rcBuildDistanceField(&ctx, *chf) ||
            !rcBuildRegions(&ctx, *chf, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea)) {
            OX_LOG_ERROR("ai", "navmesh: watershed regions failed");
            return false;
        }
        break;
    case NavPartition::Monotone:
        if (!rcBuildRegionsMonotone(&ctx, *chf, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea)) {
            OX_LOG_ERROR("ai", "navmesh: monotone regions failed");
            return false;
        }
        break;
    case NavPartition::Layers:
        if (!rcBuildLayerRegions(&ctx, *chf, cfg.borderSize, cfg.minRegionArea)) {
            OX_LOG_ERROR("ai", "navmesh: layer regions failed");
            return false;
        }
        break;
    }

    RcPtr<rcContourSet> cset(rcAllocContourSet());
    if (!cset || !rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *cset)) {
        OX_LOG_ERROR("ai", "navmesh: contours failed");
        return false;
    }
    if (cset->nconts == 0) {
        return false;
    }
    RcPtr<rcPolyMesh> pmesh(rcAllocPolyMesh());
    if (!pmesh || !rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pmesh)) {
        OX_LOG_ERROR("ai", "navmesh: poly mesh failed");
        return false;
    }
    RcPtr<rcPolyMeshDetail> dmesh(rcAllocPolyMeshDetail());
    if (!dmesh || !rcBuildPolyMeshDetail(&ctx, *pmesh, *chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *dmesh)) {
        OX_LOG_ERROR("ai", "navmesh: detail mesh failed");
        return false;
    }
    if (pmesh->npolys == 0) {
        return false;
    }
    for (int i = 0; i < pmesh->npolys; ++i) {
        if (pmesh->areas[i] == RC_WALKABLE_AREA) {
            pmesh->areas[i] = NavArea::Ground;
        }
        pmesh->flags[i] = s.areaFlags[pmesh->areas[i] % NavArea::Count];
    }

    std::vector<float> omVerts, omRad;
    std::vector<unsigned char> omDir, omArea;
    std::vector<unsigned short> omFlags;
    std::vector<unsigned int> omId;
    for (const OffMeshLink& l : input.offMeshLinks) {
        omVerts.insert(omVerts.end(), {l.start.x, l.start.y, l.start.z, l.end.x, l.end.y, l.end.z});
        omRad.push_back(l.radius);
        omDir.push_back(l.bidirectional ? DT_OFFMESH_CON_BIDIR : 0);
        omArea.push_back(l.area);
        omFlags.push_back(l.flags != 0 ? l.flags : s.areaFlags[l.area % NavArea::Count]);
        omId.push_back(l.userId);
    }

    dtNavMeshCreateParams params{};
    params.verts = pmesh->verts;
    params.vertCount = pmesh->nverts;
    params.polys = pmesh->polys;
    params.polyAreas = pmesh->areas;
    params.polyFlags = pmesh->flags;
    params.polyCount = pmesh->npolys;
    params.nvp = pmesh->nvp;
    params.detailMeshes = dmesh->meshes;
    params.detailVerts = dmesh->verts;
    params.detailVertsCount = dmesh->nverts;
    params.detailTris = dmesh->tris;
    params.detailTriCount = dmesh->ntris;
    params.offMeshConVerts = omVerts.empty() ? nullptr : omVerts.data();
    params.offMeshConRad = omRad.empty() ? nullptr : omRad.data();
    params.offMeshConDir = omDir.empty() ? nullptr : omDir.data();
    params.offMeshConAreas = omArea.empty() ? nullptr : omArea.data();
    params.offMeshConFlags = omFlags.empty() ? nullptr : omFlags.data();
    params.offMeshConUserID = omId.empty() ? nullptr : omId.data();
    params.offMeshConCount = static_cast<int>(omRad.size());
    params.walkableHeight = s.agentHeight;
    params.walkableRadius = s.agentRadius;
    params.walkableClimb = s.agentMaxClimb;
    params.tileX = tx;
    params.tileY = ty;
    params.tileLayer = 0;
    std::memcpy(params.bmin, pmesh->bmin, sizeof(params.bmin));
    std::memcpy(params.bmax, pmesh->bmax, sizeof(params.bmax));
    params.cs = cfg.cs;
    params.ch = cfg.ch;
    params.buildBvTree = true;

    unsigned char* data = nullptr;
    int size = 0;
    if (!dtCreateNavMeshData(&params, &data, &size)) {
        OX_LOG_ERROR("ai", "navmesh: dtCreateNavMeshData failed");
        return false;
    }
    *outData = data;
    *outSize = size;
    return true;
}

} // namespace detail
} // namespace ox::ai
