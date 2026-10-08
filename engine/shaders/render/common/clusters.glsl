// OxwaldEngine render: clustered light lookup (mirrors ox::render::ClusterGrid).
// LightClusters buffer: counts[clusterCount] followed by indices[clusterCount × maxLightsPerCluster]; indices are
// absolute indices into SCENE.lights (local lights only, directional lights are always evaluated).
#ifndef OX_RENDER_CLUSTERS_GLSL
#define OX_RENDER_CLUSTERS_GLSL

#include "view.glsl"

layout(buffer_reference, scalar, buffer_reference_align = 4) buffer LightClusters { uint data[]; };

uint oxClusterSlice(ViewBuffer vb, float viewDepth) {
    float s = log(max(viewDepth, 1e-4)) * vb.v.clusterDepth.x + vb.v.clusterDepth.y;
    return uint(clamp(s, 0.0, float(vb.v.clusterGrid.z - 1u)));
}

// uv: [0,1]² top-left origin (render resolution), viewDepth: positive distance.
uint oxClusterIndex(ViewBuffer vb, vec2 uv, float viewDepth) {
    uvec2 tile = min(uvec2(uv * vec2(vb.v.clusterGrid.xy)), vb.v.clusterGrid.xy - 1u);
    uint slice = oxClusterSlice(vb, viewDepth);
    return tile.x + tile.y * vb.v.clusterGrid.x + slice * vb.v.clusterGrid.x * vb.v.clusterGrid.y;
}

uint oxClusterLightCount(ViewBuffer vb, uint cluster) {
    return min(vb.v.clusters.data[cluster], vb.v.clusterGrid.w);
}

uint oxClusterLight(ViewBuffer vb, uint cluster, uint i) {
    uint clusterCount = vb.v.clusterGrid.x * vb.v.clusterGrid.y * vb.v.clusterGrid.z;
    return vb.v.clusters.data[clusterCount + cluster * vb.v.clusterGrid.w + i];
}

#endif
