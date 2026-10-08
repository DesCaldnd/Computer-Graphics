// OxwaldEngine render: shadow sampling — hardware depth compare (sampler2DShadow / sampler2DArrayShadow), Poisson
// PCF rotated per pixel by interleaved gradient noise, PCSS (blocker search + penumbra estimate).
//
//   float sun   = oxSunShadow(pc.view, worldPos, N, NdotL, viewDepth, gl_FragCoord.xy);          // CSM
//   float spot  = oxSpotShadow(pc.view, SCENE.shadows.s[light.shadowIndex], worldPos, N, gl_FragCoord.xy);
//   float point = oxPointShadow(pc.view, SCENE.shadows.s[i], light.position, worldPos, N, gl_FragCoord.xy);
//
// Shadow maps are always read through oxShadowTextures2D/oxShadowTextures2DArray (see bindless.glsl: Metal types
// shadow-sampled arrays as depth textures). Depth is reversed-Z everywhere: a receiver is lit when its depth >= the stored (closest) depth.
// Point light faces are slices of the PointShadows 2D array (layer = cubeLayer · 6 + face), rendered with the
// face matrices of ox::render::cubeFaceViewProj (guard-banded FOV so PCF kernels stay inside a face).
#ifndef OX_RENDER_SHADOWS_GLSL
#define OX_RENDER_SHADOWS_GLSL

#include "math.glsl"
#include "scene.glsl"

const uint OX_SHADOW_MAX_TAPS = 16u;

float oxShadowCompare2D(uint tex, vec2 uv, float ref) {
    return OX_SAMPLE_SHADOW(tex, vec3(uv, ref));
}
float oxShadowCompareArray(uint tex, vec2 uv, float layer, float ref) {
    return OX_SAMPLE_SHADOW_ARRAY(tex, vec4(uv, layer, ref));
}
float oxShadowDepth2D(uint tex, vec2 uv) {
    return textureLod(sampler2D(oxShadowTextures2D[nonuniformEXT(tex)], OX_SAMPLER(OX_SAMPLER_NEAREST_CLAMP)), uv, 0.0).r;
}
float oxShadowDepthArray(uint tex, vec2 uv, float layer) {
    return textureLod(sampler2DArray(oxShadowTextures2DArray[nonuniformEXT(tex)], OX_SAMPLER(OX_SAMPLER_NEAREST_CLAMP)), vec3(uv, layer), 0.0).r;
}

// --- 2D (spot atlas): uv clamped to the tile rect (xy min, zw max) ---

float oxPcf2D(uint tex, vec2 uv, float ref, float radiusTexels, float texel, vec4 rect, uint taps, float noise) {
    if (taps == 0u || radiusTexels <= 0.0) return oxShadowCompare2D(tex, clamp(uv, rect.xy, rect.zw), ref);
    mat2 rot = oxRotation2D(noise * OX_TWO_PI);
    float sum = 0.0;
    uint n = min(taps, OX_SHADOW_MAX_TAPS);
    for (uint i = 0u; i < n; ++i) {
        vec2 o = rot * OX_POISSON16[i] * radiusTexels * texel;
        sum += oxShadowCompare2D(tex, clamp(uv + o, rect.xy, rect.zw), ref);
    }
    return sum / float(n);
}

// Average depth of the texels closer to the light than `ref`; returns the blocker count.
float oxBlockerSearch2D(uint tex, vec2 uv, float ref, float radiusTexels, float texel, vec4 rect, float noise,
                        out float avgDepth) {
    mat2 rot = oxRotation2D(noise * OX_TWO_PI);
    float sum = 0.0, count = 0.0;
    for (uint i = 0u; i < 16u; ++i) {
        float d = oxShadowDepth2D(tex, clamp(uv + rot * OX_POISSON16[i] * radiusTexels * texel, rect.xy, rect.zw));
        if (d > ref) { sum += d; count += 1.0; }
    }
    avgDepth = count > 0.0 ? sum / count : 0.0;
    return count;
}

// --- 2D arrays (cascades, point faces) ---

float oxPcfArray(uint tex, vec2 uv, float layer, float ref, float radiusTexels, float texel, uint taps, float noise) {
    if (taps == 0u || radiusTexels <= 0.0) return oxShadowCompareArray(tex, uv, layer, ref);
    mat2 rot = oxRotation2D(noise * OX_TWO_PI);
    float sum = 0.0;
    uint n = min(taps, OX_SHADOW_MAX_TAPS);
    for (uint i = 0u; i < n; ++i) {
        sum += oxShadowCompareArray(tex, uv + rot * OX_POISSON16[i] * radiusTexels * texel, layer, ref);
    }
    return sum / float(n);
}

float oxBlockerSearchArray(uint tex, vec2 uv, float layer, float ref, float radiusTexels, float texel, float noise,
                           out float avgDepth) {
    mat2 rot = oxRotation2D(noise * OX_TWO_PI);
    float sum = 0.0, count = 0.0;
    for (uint i = 0u; i < 16u; ++i) {
        float d = oxShadowDepthArray(tex, uv + rot * OX_POISSON16[i] * radiusTexels * texel, layer);
        if (d > ref) { sum += d; count += 1.0; }
    }
    avgDepth = count > 0.0 ? sum / count : 0.0;
    return count;
}

// Reversed-Z perspective device depth → linear distance along the light axis.
float oxShadowLinearDepth(float d, float n, float f) { return n * f / (d * (f - n) + n); }

// --- directional light (cascaded shadow maps) ---

// Cascade index for a view depth (cascadeCount when beyond the shadow distance).
uint oxSelectCascade(ViewBuffer vb, float viewDepth) {
    uint count = vb.v.cascadeCount;
    vec4 splits = vb.v.cascadeSplits;
    for (uint i = 0u; i < count; ++i) {
        if (viewDepth < splits[i]) return i;
    }
    return count;
}

float oxSampleCascade(ViewBuffer vb, uint cascade, vec3 worldPos, vec3 N, float noise) {
    float texelWorld = vb.v.cascadeTexelWorld[cascade];
    vec3 p = worldPos + N * vb.v.sunNormalBias * texelWorld;
    vec4 c = vb.v.cascadeViewProj[cascade] * vec4(p, 1.0);
    vec3 ndc = c.xyz / c.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || ndc.z < 0.0) return 1.0;
    float ref = ndc.z + vb.v.sunBias;
    float texel = 1.0 / vb.v.csmResolution;
    float radius = vb.v.shadowParams.y;
    uint taps = uint(vb.v.shadowParams.w);
    uint tex = vb.v.cascadeTexture;
    if (vb.v.shadowParams.z > 0.5) {
        // PCSS: penumbra = occluder distance × tan(angular radius of the sun).
        float tanAngle = tan(clamp(vb.v.sunAngularRadius, 0.0, 0.2));
        float depthRange = vb.v.cascadeDepthRange[cascade];
        float search = clamp(10.0 * tanAngle / texelWorld, 1.5, 16.0);
        float avg;
        float blockers = oxBlockerSearchArray(tex, uv, float(cascade), ref, search, texel, noise, avg);
        if (blockers < 1.0) return 1.0;
        float occluderDistance = max(avg - ref, 0.0) * depthRange;
        radius = clamp(occluderDistance * tanAngle / texelWorld, 1.0, 24.0);
    }
    return oxPcfArray(tex, uv, float(cascade), ref, radius, texel, taps, noise);
}

// Sun visibility for a world position. viewDepth = positive view-space distance of the shaded point.
float oxSunShadow(ViewBuffer vb, vec3 worldPos, vec3 N, float viewDepth, vec2 pixel) {
    if (vb.v.cascadeTexture == OX_INVALID_INDEX || vb.v.cascadeCount == 0u) return 1.0;
    uint cascade = oxSelectCascade(vb, viewDepth);
    uint count = vb.v.cascadeCount;
    if (cascade >= count) return 1.0;
    float noise = oxInterleavedGradientNoise(pixel + float(vb.v.frameIndex % 8u) * 5.588238);
    float s = oxSampleCascade(vb, cascade, worldPos, N, noise);
    float splitFar = vb.v.cascadeSplits[cascade];
    float splitNear = cascade == 0u ? 0.0 : vb.v.cascadeSplits[cascade - 1u];
    float blendStart = splitFar - (splitFar - splitNear) * vb.v.shadowParams.x;
    if (viewDepth > blendStart) {
        float t = (viewDepth - blendStart) / max(splitFar - blendStart, 1e-4);
        float next = cascade + 1u < count ? oxSampleCascade(vb, cascade + 1u, worldPos, N, noise) : 1.0;
        s = mix(s, next, t);
    }
    return s;
}

// --- spot lights (atlas) ---

float oxSpotShadow(ViewBuffer vb, Shadow sh, vec3 worldPos, vec3 N, vec2 pixel) {
    uint tex = vb.v.shadowAtlas;
    if (tex == OX_INVALID_INDEX) return 1.0;
    vec4 c0 = sh.viewProj * vec4(worldPos, 1.0);
    float texelWorld = max(c0.w, 0.0) * sh.texelSize;
    vec4 c = sh.viewProj * vec4(worldPos + N * sh.normalBias * texelWorld, 1.0);
    if (c.w <= 0.0) return 1.0;
    vec3 ndc = c.xyz / c.w;
    vec2 local = ndc.xy * 0.5 + 0.5;
    if (any(lessThan(local, vec2(0.0))) || any(greaterThan(local, vec2(1.0))) || ndc.z < 0.0) return 1.0;
    float atlasTexel = 1.0 / float(vb.v.shadowAtlasSize);
    vec2 uv = sh.atlasRect.xy + local * sh.atlasRect.zw;
    vec4 rect = vec4(sh.atlasRect.xy + 0.5 * atlasTexel, sh.atlasRect.xy + sh.atlasRect.zw - 0.5 * atlasTexel);
    float ref = ndc.z + sh.bias;
    float noise = oxInterleavedGradientNoise(pixel + float(vb.v.frameIndex % 8u) * 5.588238);
    float radius = vb.v.shadowParams.y;
    uint taps = uint(vb.v.shadowParams.w);
    if (vb.v.shadowParams.z > 0.5 && sh.lightSize > 0.0) {
        float avg;
        float blockers = oxBlockerSearch2D(tex, uv, ref, 8.0, atlasTexel, rect, noise, avg);
        if (blockers < 1.0) return 1.0;
        float zR = oxShadowLinearDepth(ref, sh.nearPlane, sh.farPlane);
        float zB = oxShadowLinearDepth(avg, sh.nearPlane, sh.farPlane);
        radius = clamp(sh.lightSize * (zR - zB) / max(zB * zR * sh.texelSize, 1e-6), 1.0, 16.0);
    }
    return oxPcf2D(tex, uv, ref, radius, atlasTexel, rect, taps, noise);
}

// --- point lights (6 faces per light in the PointShadows array) ---
// Face orientation table: must match ox::render::cubeFaceForward / cubeFaceUp.
const vec3 OX_CUBE_FORWARD[6] = vec3[](vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 1, 0), vec3(0, -1, 0), vec3(0, 0, 1),
                                        vec3(0, 0, -1));
const vec3 OX_CUBE_UP[6] = vec3[](vec3(0, -1, 0), vec3(0, -1, 0), vec3(0, 0, 1), vec3(0, 0, -1), vec3(0, -1, 0),
                                   vec3(0, -1, 0));

uint oxCubeFace(vec3 d) {
    vec3 a = abs(d);
    if (a.x >= a.y && a.x >= a.z) return d.x >= 0.0 ? 0u : 1u;
    if (a.y >= a.z) return d.y >= 0.0 ? 2u : 3u;
    return d.z >= 0.0 ? 4u : 5u;
}

// For point shadows atlasRect.x = tan(face fov / 2) (guard band included).
float oxPointShadow(ViewBuffer vb, Shadow sh, vec3 lightPos, vec3 worldPos, vec3 N, vec2 pixel) {
    uint tex = vb.v.pointShadows;
    if (tex == OX_INVALID_INDEX) return 1.0;
    vec3 d0 = worldPos - lightPos;
    uint face0 = oxCubeFace(d0);
    float z0 = max(dot(d0, OX_CUBE_FORWARD[face0]), 1e-4);
    vec3 d = d0 + N * sh.normalBias * z0 * sh.texelSize;
    uint face = oxCubeFace(d);
    vec3 f = OX_CUBE_FORWARD[face];
    vec3 u = OX_CUBE_UP[face];
    vec3 s = normalize(cross(f, u));
    float zf = max(dot(d, f), 1e-4);
    float tanHalf = sh.atlasRect.x;
    vec2 ndc = vec2(dot(d, s), dot(d, u)) / (zf * tanHalf);
    vec2 uv = ndc * 0.5 + 0.5;
    float n = sh.nearPlane, fa = sh.farPlane;
    float depth = n * (fa - zf) / (zf * (fa - n));
    if (depth < 0.0) return 1.0;
    float ref = depth + sh.bias;
    float layer = float(sh.cubeLayer * 6u + face);
    float texel = sh.atlasRect.y > 0.0 ? 1.0 / sh.atlasRect.y : 1.0 / 512.0;
    float noise = oxInterleavedGradientNoise(pixel + float(vb.v.frameIndex % 8u) * 5.588238);
    float radius = vb.v.shadowParams.y;
    uint taps = uint(vb.v.shadowParams.w);
    if (vb.v.shadowParams.z > 0.5 && sh.lightSize > 0.0) {
        float avg;
        float blockers = oxBlockerSearchArray(tex, uv, layer, ref, 8.0, texel, noise, avg);
        if (blockers < 1.0) return 1.0;
        float zB = oxShadowLinearDepth(avg, n, fa);
        radius = clamp(sh.lightSize * (zf - zB) / max(zB * zf * sh.texelSize, 1e-6), 1.0, 16.0);
    }
    return oxPcfArray(tex, uv, layer, ref, radius, texel, taps, noise);
}

#endif
