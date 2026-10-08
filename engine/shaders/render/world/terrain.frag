#version 460
// Terrain forward shading: splat blending of up to 8 PBR layers (scene materials, bindless textures) with
// height-based blending, triplanar projection on steep slopes, tiling breakup and macro variation. Without layers
// the terrain is coloured procedurally by slope and height. Depth EQUAL against the terrain prepass.
#include "terrain_common.glsl"
#include "world_shading.glsl"

// inputs: shadowMask, ao, reflections, indirectDiffuse
OX_RENDER_PUSH(TerrainParamsBuffer params; TerrainPatchBuffer patches; TerrainGridBuffer grid; OxWorldMatrixBuffer matrices; uint inputs[4];);

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec4 vCurClip;
layout(location = 2) in vec4 vPrevClip;

layout(location = 0) out vec4 outColor;

struct LayerSample {
    vec3 albedo;
    vec3 normal; // world-space perturbation added to the macro normal (xz + y)
    float roughness;
    float metallic;
    float ao;
    float height; // [0,1] proxy for height blending
};

vec4 sampleGrad(uint tex, uint smp, vec2 uv, vec2 gx, vec2 gy) {
    return textureGrad(sampler2D(OX_TEX2D(tex), OX_SAMPLER(smp)), uv, gx, gy);
}

// One planar projection of a layer. uv in material space, (gx, gy) its screen derivatives.
LayerSample samplePlanar(Material m, vec2 uv, vec2 gx, vec2 gy) {
    LayerSample o;
    uv = uv * m.uvTiling + m.uvOffset;
    gx *= m.uvTiling;
    gy *= m.uvTiling;
    vec4 albedo = m.baseColor;
    if (m.albedoTexture != OX_INVALID_INDEX) albedo *= sampleGrad(m.albedoTexture, m.samplerIndex, uv, gx, gy);
    o.albedo = albedo.rgb;
    o.roughness = m.roughness;
    o.metallic = m.metallic;
    o.ao = 1.0;
    o.height = oxLuminance(albedo.rgb) / max(oxLuminance(m.baseColor.rgb), 1e-3);
    if (m.ormTexture != OX_INVALID_INDEX) {
        vec3 orm = sampleGrad(m.ormTexture, m.samplerIndex, uv, gx, gy).rgb;
        o.ao = mix(1.0, orm.r, m.occlusionStrength);
        o.roughness *= orm.g;
        o.metallic *= orm.b;
        o.height = orm.r; // cavities are dark in the AO map: a decent height proxy
    }
    o.normal = vec3(0.0);
    if (m.normalTexture != OX_INVALID_INDEX) {
        vec2 xy = (sampleGrad(m.normalTexture, m.samplerIndex, uv, gx, gy).rg * 2.0 - 1.0) * m.normalStrength;
        o.normal = vec3(xy.x, 0.0, xy.y); // tangent-space XY on the projection plane (mapped by the caller)
    }
    o.height = clamp(o.height, 0.0, 1.0);
    return o;
}

LayerSample mixSample(LayerSample a, LayerSample b, float t) {
    LayerSample o;
    o.albedo = mix(a.albedo, b.albedo, t);
    o.normal = mix(a.normal, b.normal, t);
    o.roughness = mix(a.roughness, b.roughness, t);
    o.metallic = mix(a.metallic, b.metallic, t);
    o.ao = mix(a.ao, b.ao, t);
    o.height = mix(a.height, b.height, t);
    return o;
}

// Top projection with tiling breakup: a second sample, rotated and scaled, blended by low-frequency noise.
LayerSample sampleTop(Material m, vec2 uv, vec2 gx, vec2 gy, float breakup, float breakupNoise) {
    LayerSample a = samplePlanar(m, uv, gx, gy);
    if (breakup <= 0.0) return a;
    const mat2 rot = mat2(0.7374, 0.6755, -0.6755, 0.7374); // 42.5°
    const float scale = 0.73;
    LayerSample b = samplePlanar(m, rot * uv * scale + vec2(0.37, 0.61), rot * gx * scale, rot * gy * scale);
    float t = smoothstep(0.35, 0.65, breakupNoise) * breakup;
    LayerSample r = mixSample(a, b, t);
    r.normal.xz = mix(a.normal.xz, rot * b.normal.xz, t);
    return r;
}

LayerSample sampleLayer(Material m, vec3 wp, vec3 dpdx, vec3 dpdy, vec3 macroN, vec3 tri, float invTile, float breakup,
                        float breakupNoise) {
    vec2 uvTop = wp.xz * invTile;
    LayerSample top = sampleTop(m, uvTop, dpdx.xz * invTile, dpdy.xz * invTile, breakup, breakupNoise);
    top.normal = vec3(top.normal.x, 0.0, top.normal.z);
    if (tri.y >= 0.999) return top;
    // Side projections (X: zy plane, Z: xy plane); tangent-space xy maps to the projection axes.
    LayerSample sx = samplePlanar(m, wp.zy * invTile, dpdx.zy * invTile, dpdy.zy * invTile);
    sx.normal = vec3(0.0, sx.normal.z, sx.normal.x);
    LayerSample sz = samplePlanar(m, wp.xy * invTile, dpdx.xy * invTile, dpdy.xy * invTile);
    sz.normal = vec3(sz.normal.x, sz.normal.z, 0.0);
    LayerSample o;
    o.albedo = top.albedo * tri.y + sx.albedo * tri.x + sz.albedo * tri.z;
    o.normal = top.normal * tri.y + sx.normal * tri.x + sz.normal * tri.z;
    o.roughness = top.roughness * tri.y + sx.roughness * tri.x + sz.roughness * tri.z;
    o.metallic = top.metallic * tri.y + sx.metallic * tri.x + sz.metallic * tri.z;
    o.ao = top.ao * tri.y + sx.ao * tri.x + sz.ao * tri.z;
    o.height = top.height * tri.y + sx.height * tri.x + sz.height * tri.z;
    return o;
}

void main() {
    TerrainParamsBuffer tp = pc.params;
    vec3 wp = vWorldPos;
    vec3 macroN = oxTerrainMacroNormal(tp, wp.xz);
    vec3 camPos = VIEW.cameraPosition.xyz;
    float dist = distance(camPos, wp);
    vec3 dpdx = dFdx(wp), dpdy = dFdy(wp);

    vec3 albedo;
    vec3 N = macroN;
    float roughness, metallic = 0.0, ao = 1.0;
    uint flags = tp.p.flags;
    uint layerCount = uint(tp.p.shading1.z);
    if ((flags & OX_TERRAIN_SPLAT) != 0u && layerCount > 0u) {
        // --- splat weights ---
        vec2 suv = oxTerrainSampleUv(wp.xz, tp.p.splatRect.xy, tp.p.splatRect.z / max(tp.p.splatRect.w - 1.0, 1.0),
                                     tp.p.splatRect.w);
        vec4 s0 = OX_SAMPLE_2D_LOD(tp.p.splat0, OX_SAMPLER_LINEAR_CLAMP, suv, 0.0);
        vec4 s1 = layerCount > 4u ? OX_SAMPLE_2D_LOD(tp.p.splat1, OX_SAMPLER_LINEAR_CLAMP, suv, 0.0) : vec4(0.0);
        float w[8] = float[](s0.x, s0.y, s0.z, s0.w, s1.x, s1.y, s1.z, s1.w);
        for (uint i = layerCount; i < 8u; ++i) w[i] = 0.0;
        // Keep the strongest r.Terrain.MaxLayers layers.
        uint maxLayers = uint(tp.p.shading1.y);
        uint activeLayer[8];
        uint activeCount = 0u;
        float used[8] = w;
        for (uint n = 0u; n < min(maxLayers, 8u); ++n) {
            uint best = 8u;
            float bw = 1.0 / 255.0;
            for (uint i = 0u; i < 8u; ++i) {
                if (used[i] > bw) {
                    bw = used[i];
                    best = i;
                }
            }
            if (best == 8u) break;
            used[best] = 0.0;
            activeLayer[activeCount++] = best;
        }
        if (activeCount == 0u) activeLayer[activeCount++] = 0u;

        // --- projection weights ---
        float cosTri = tp.p.shading0.x;
        vec3 tri = vec3(0.0, 1.0, 0.0);
        if ((flags & OX_TERRAIN_TRIPLANAR) != 0u && macroN.y < cosTri) {
            vec3 a = pow(abs(macroN), vec3(4.0));
            a /= (a.x + a.y + a.z);
            float t = smoothstep(cosTri, cosTri - 0.15, macroN.y);
            tri = mix(vec3(0.0, 1.0, 0.0), a, t);
        }
        float invTile = tp.p.shading1.x;
        float breakup = tp.p.shading0.w;
        float breakupNoise = oxWorldValueNoise(wp.xz * 0.045);

        LayerSample ls[8];
        float hw[8];
        float sumW = 0.0;
        float maxV = 0.0;
        float heightBlend = tp.p.shading0.y;
        for (uint k = 0u; k < activeCount; ++k) {
            uint i = activeLayer[k];
            Material m = SCENE.materials.m[tp.p.layerMaterial[i]];
            ls[k] = sampleLayer(m, wp, dpdx, dpdy, macroN, tri, invTile, breakup, breakupNoise);
            hw[k] = w[i];
            sumW += w[i];
            maxV = max(maxV, w[i] + ls[k].height * heightBlend);
        }
        // Height-based blending: the layer whose (weight + height) rises highest wins near the transition.
        float depth = max(heightBlend * 0.5, 1e-3);
        float bsum = 0.0;
        for (uint k = 0u; k < activeCount; ++k) {
            float plain = hw[k] / max(sumW, 1e-4);
            float hb = max(hw[k] + ls[k].height * heightBlend - (maxV - depth), 0.0);
            hw[k] = heightBlend > 0.0 ? hb : plain;
            bsum += hw[k];
        }
        albedo = vec3(0.0);
        vec3 nPert = vec3(0.0);
        roughness = 0.0;
        for (uint k = 0u; k < activeCount; ++k) {
            float b = hw[k] / max(bsum, 1e-5);
            albedo += ls[k].albedo * b;
            nPert += ls[k].normal * b;
            roughness += ls[k].roughness * b;
            metallic += ls[k].metallic * b;
            ao += (ls[k].ao - 1.0) * b;
        }
        N = normalize(macroN + nPert);
    } else {
        // Procedural fallback: grass on flats, rock on slopes, snow on the highest 15 %.
        float h01 = (wp.y - tp.p.heightParams.y) / max(tp.p.heightParams.x, 1e-3);
        float n = oxWorldFbm(wp.xz * 0.08);
        vec3 grass = mix(vec3(0.10, 0.17, 0.04), vec3(0.20, 0.26, 0.07), n);
        vec3 rock = mix(vec3(0.22, 0.20, 0.18), vec3(0.34, 0.32, 0.29), oxWorldFbm(wp.xz * 0.3));
        float slope = smoothstep(0.82, 0.68, macroN.y);
        albedo = mix(grass, rock, slope);
        float snow = smoothstep(0.82, 0.9, h01 + n * 0.06) * smoothstep(0.55, 0.75, macroN.y);
        albedo = mix(albedo, vec3(0.85, 0.87, 0.9), snow);
        roughness = mix(0.9, 0.55, snow);
    }

    // Macro variation: low-frequency brightness / hue shift that hides tiling at a distance.
    float macro = tp.p.shading0.z;
    if (macro > 0.0) {
        float v = oxWorldFbm(wp.xz * 0.012) - 0.5;
        float far = smoothstep(10.0, 80.0, dist);
        albedo *= 1.0 + v * macro * (0.6 + 0.4 * far);
        albedo = mix(albedo, albedo * vec3(1.04, 1.0, 0.94), (oxWorldValueNoise(wp.xz * 0.004) - 0.5) * macro);
    }

    OxSurface s;
    s.position = wp;
    s.normal = N;
    s.geometricNormal = macroN;
    s.view = (VIEW.flags & OX_VIEW_ORTHOGRAPHIC) != 0u ? normalize(VIEW.invView[2].xyz) : normalize(camPos - wp);
    s.baseColor = clamp(albedo, 0.0, 1.0);
    s.alpha = 1.0;
    s.metallic = clamp(metallic, 0.0, 1.0);
    s.perceptualRoughness = clamp(roughness, 0.0, 1.0);
    s.occlusion = clamp(ao, 0.0, 1.0);
    s.emissive = vec3(0.0);
    oxSurfaceFinalize(s);
    OxWorldInputs inp = OxWorldInputs(pc.inputs[0], pc.inputs[1], pc.inputs[2], pc.inputs[3]);
    outColor = oxWorldShade(pc.view, pc.scene, s, gl_FragCoord.xy, inp, vec3(0.0), true);
}
