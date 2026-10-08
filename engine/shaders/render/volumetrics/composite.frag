#version 460
// Volumetrics composite onto SceneColorHDR (after opaque + sky, before translucency). Per pixel, near to far:
// froxel fog up to the surface, analytic height fog beyond the froxel grid (sky: up to the sky fog distance =
// aerial perspective / horizon haze), then the clouds (depth-aware upsampled from cloud resolution).
// Output (rgb = in-scatter, a = transmittance) is blended as dst · a + rgb.
#include <render/volumetrics/fog_sample.glsl>

OX_RENDER_PUSH(uint depth; uint fog; uint cloudColor; uint cloudData; uint flags;);

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

const uint FLAG_CLOUDS = 1u;
const uint FLAG_ANALYTIC = 2u;

// Bilateral upsample of the cloud buffer: bilinear weights × similarity of the depth the trace was limited by.
vec4 sampleClouds(vec2 pixelUv, float sceneDepth) {
    vec2 size = vec2(textureSize(oxTextures2D[nonuniformEXT(pc.cloudColor)], 0));
    vec2 p = pixelUv * size - 0.5;
    ivec2 i = ivec2(floor(p));
    vec2 f = p - vec2(i);
    vec4 sum = vec4(0.0);
    float wsum = 0.0;
    vec4 best = vec4(0.0, 0.0, 0.0, 1.0);
    float bestDiff = 1e30;
    for (int k = 0; k < 4; ++k) {
        ivec2 o = ivec2(k & 1, k >> 1);
        ivec2 c = clamp(i + o, ivec2(0), ivec2(size) - 1);
        vec4 col = texelFetch(oxTextures2D[nonuniformEXT(pc.cloudColor)], c, 0);
        float d = texelFetch(oxTextures2D[nonuniformEXT(pc.cloudData)], c, 0).y;
        float bil = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
        float diff = abs(d - sceneDepth) / max(sceneDepth, 1.0);
        float w = bil / (1e-3 + diff * 20.0);
        sum += col * w;
        wsum += w;
        if (diff < bestDiff) {
            bestDiff = diff;
            best = col;
        }
    }
    return bestDiff > 0.5 ? best : sum / max(wsum, 1e-6);
}

void main() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    vec2 puv = gl_FragCoord.xy * VIEW.renderSize.zw;
    float device = OX_FETCH_2D(pc.depth, px, 0).r;
    bool sky = device <= 0.0;
    vec3 cam = VIEW.cameraPosition.xyz;
    vec3 dir = oxViewRay(pc.view, puv);
    float rayScale = oxFogRayScale(pc.view, puv);
    float viewDepth, dist;
    if (sky) {
        dist = VIEW.volumetricFogLighting.w;
        viewDepth = dist / rayScale;
    } else {
        viewDepth = oxLinearDepth(pc.view, device);
        dist = viewDepth * rayScale;
    }

    // Froxels (+ analytic height fog beyond the grid).
    vec4 fog = vec4(0.0, 0.0, 0.0, 1.0);
    if (oxFroxelFogActive(pc.view)) {
        fog = oxSampleVolumetricFog(pc.view, pc.fog, puv, viewDepth);
        if ((pc.flags & FLAG_ANALYTIC) != 0u) {
            vec4 far = oxAnalyticHeightFog(pc.view, pc.scene, dir, VIEW.volumetricFogGrid.w * rayScale, dist);
            fog = vec4(fog.rgb + fog.a * far.rgb, fog.a * far.a);
        }
    }

    // Clouds behind the fog.
    if ((pc.flags & FLAG_CLOUDS) != 0u) {
        vec4 cl = sampleClouds(puv, sky ? OX_FOG_SKY_DEPTH : viewDepth);
        fog = vec4(fog.rgb + fog.a * cl.rgb, fog.a * cl.a);
    }
    outColor = fog;
}
