#version 460
// Water surface shading: Gerstner + detail normals, refraction of the bottom with depth-based absorption and
// in-scattering, reflection (planar reflection input → SSR on SceneColorRefraction → prefiltered environment),
// Schlick Fresnel (F0 = 0.02) with total internal reflection seen from below, shore and crest foam, fog.
#include "water_push.glsl"
#include "common.glsl"
#ifdef OX_HAS_PLANAR_REFLECTIONS
#include <render/reflections/planar.glsl>
#endif

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec2 vRest;
layout(location = 2) in float vCrest;
layout(location = 0) out vec4 outColor;

const float kWaterIor = 1.333;

vec3 detailNormal(vec2 xz, float time) {
    if (WATER.normalMap == OX_INVALID_INDEX) return vec3(0.0, 1.0, 0.0);
    float scale = WATER.detail.y;
    vec2 scroll = vec2(0.8, 0.6) * WATER.detail.z * time;
    vec2 a = OX_SAMPLE_2D(WATER.normalMap, OX_SAMPLER_ANISO_REPEAT, xz * scale + scroll).xy * 2.0 - 1.0;
    vec2 b = OX_SAMPLE_2D(WATER.normalMap, OX_SAMPLER_ANISO_REPEAT, xz * scale * 1.73 - scroll.yx * 1.3).xy * 2.0 - 1.0;
    vec2 n = (a + b) * WATER.detail.x;
    return normalize(vec3(n.x, 1.0, n.y));
}

// Screen-space reflection against the opaque depth copy (exponentially growing steps + binary refinement);
// returns rgb (pre-exposed) and confidence.
vec4 traceSsr(vec3 origin, vec3 dir, float roughness) {
    if (pc.ssrSteps == 0u || dir.y < -0.2) return vec4(0.0);
    float t = 0.1;
    float stepLen = 0.3;
    float prevT = 0.0;
    for (uint i = 0u; i < pc.ssrSteps; ++i) {
        vec3 p = origin + dir * t;
        float d;
        vec2 uv = oxProjectToUv(VIEW.viewProj, p, d);
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || d <= 0.0) break;
        float diff = oxTranslucencyViewDepth(pc.view, p) - oxSceneLinearDepth(pc.view, pc.sceneDepth, uv);
        if (diff > 0.0) {
            if (diff > stepLen * 2.0 + 0.5) break; // passed behind a foreground object
            float lo = prevT, hi = t;
            for (int r = 0; r < 4; ++r) {
                float mid = 0.5 * (lo + hi);
                vec3 q = origin + dir * mid;
                vec2 quv = oxProjectToUv(VIEW.viewProj, q, d);
                if (oxTranslucencyViewDepth(pc.view, q) > oxSceneLinearDepth(pc.view, pc.sceneDepth, quv)) hi = mid;
                else lo = mid;
            }
            uv = oxProjectToUv(VIEW.viewProj, origin + dir * hi, d);
            vec2 edge = min(uv, 1.0 - uv);
            float fade = clamp(min(edge.x, edge.y) * 10.0, 0.0, 1.0) * (1.0 - float(i) / float(pc.ssrSteps));
            return vec4(oxSampleRefraction(pc.refraction, pc.refractionMips, uv, roughness), fade);
        }
        prevT = t;
        t += stepLen;
        stepLen *= 1.4;
    }
    return vec4(0.0);
}

float waterSunShadow(vec3 P, float viewDepth) {
    if (VIEW.cascadeTexture == OX_INVALID_INDEX || VIEW.cascadeCount == 0u) return 1.0;
    uint c = oxSelectCascade(pc.view, viewDepth);
    if (c >= VIEW.cascadeCount) return 1.0;
    vec4 clip = VIEW.cascadeViewProj[c] * vec4(P + vec3(0.0, 1.0, 0.0) * VIEW.sunNormalBias * VIEW.cascadeTexelWorld[c], 1.0);
    vec3 ndc = clip.xyz / clip.w;
    vec2 suv = ndc.xy * 0.5 + 0.5;
    if (any(lessThan(suv, vec2(0.0))) || any(greaterThan(suv, vec2(1.0))) || ndc.z < 0.0) return 1.0;
    return oxShadowCompareArray(VIEW.cascadeTexture, suv, float(c), ndc.z + VIEW.sunBias);
}

void main() {
    float time = oxWaterTime(pc.water);
    if (!oxWaterInside(pc.water, vRest)) discard;
    vec3 Ng = oxWaterNormal(pc.water, vRest, time);
    vec3 Nd = detailNormal(vRest, time);
    // Whiteout blend of the detail normal onto the wave normal.
    vec3 N = normalize(vec3(Ng.x + Nd.x, Ng.y * Nd.y, Ng.z + Nd.z));
    bool below = !gl_FrontFacing; // the grid faces +Y: back faces are seen from under the water
    if (below) N = -N;

    vec3 P = vWorldPos;
    vec3 V = oxTranslucencyViewVector(pc.view, P);
    float viewDepth = oxTranslucencyViewDepth(pc.view, P);
    vec2 uv = gl_FragCoord.xy * VIEW.renderSize.zw;
    float rayScale = length(P - VIEW.cameraPosition.xyz) / max(viewDepth, 1e-4); // view distance per linear depth
    float roughness = max(WATER.scatter.w, 0.02);

    // --- refraction of what is behind / below the surface ---
    vec3 Nv = mat3(VIEW.view) * N;
    float sceneLin = oxSceneLinearDepth(pc.view, pc.sceneDepth, uv);
    float behind = max(sceneLin - viewDepth, 0.0);
    vec2 ruv = uv + Nv.xy * vec2(1.0, -1.0) * WATER.absorption.w * clamp(behind, 0.0, 1.0) * (below ? -1.0 : 1.0);
    ruv = clamp(ruv, vec2(0.0), vec2(1.0));
    float rLin = oxSceneLinearDepth(pc.view, pc.sceneDepth, ruv);
    if (rLin < viewDepth) {
        ruv = uv;
        rLin = sceneLin;
    }
    float path = max(rLin - viewDepth, 0.0) * rayScale; // metres of water along the view ray
    vec3 bottom = oxSampleRefraction(pc.refraction, pc.refractionMips, ruv, 0.0) / max(VIEW.preExposure, 1e-12);

    // Highlights: directional lights (sun with a cheap 2×2 hardware-PCF cascade lookup: water is large on screen)
    // and the clustered local lights, specular only (F0 = 0.02).
    OxSurface s;
    s.position = P;
    s.normal = N;
    s.geometricNormal = below ? vec3(0.0, -1.0, 0.0) : vec3(0.0, 1.0, 0.0);
    s.view = V;
    s.baseColor = vec3(0.0);
    s.alpha = 1.0;
    s.metallic = 0.0;
    s.perceptualRoughness = roughness;
    s.occlusion = 1.0;
    s.emissive = vec3(0.0);
    oxSurfaceFinalize(s);
    s.f0 = vec3(0.02);
    float sunShadow = waterSunShadow(P, viewDepth);
    vec3 specular = vec3(0.0);
    if (!below) {
        uint dirCount = SCENE.directionalLightCount;
        for (uint i = 0u; i < dirCount; ++i) {
            Light l = SCENE.lights.l[i];
            float vis = int(i) == VIEW.sunLight ? sunShadow : 1.0;
            specular += oxBrdfDirect(s, -l.direction, vec3(1.0)) * l.color * vis;
        }
        if (VIEW.lightClusterCount > 0u) {
            uint cluster = oxClusterIndex(pc.view, uv, viewDepth);
            uint count = oxClusterLightCount(pc.view, cluster);
            for (uint i = 0u; i < count; ++i) {
                Light l = SCENE.lights.l[oxClusterLight(pc.view, cluster, i)];
                vec3 toLight = l.position - P;
                float d2 = dot(toLight, toLight);
                if (d2 > l.range * l.range) continue;
                vec3 L = toLight * inversesqrt(max(d2, 1e-8));
                float att = oxDistanceAttenuation(d2, l.range);
                if (l.type == OX_LIGHT_SPOT) att *= oxSpotAttenuation(L, l.direction, l.spotScale, l.spotOffset);
                specular += oxBrdfDirect(s, L, vec3(1.0)) * l.color * att;
            }
        }
    }

    // Light reaching the water body: sun (shadowed) + ambient sky irradiance.
    vec4 sh[9];
    OxSHBuffer shb = VIEW.irradianceSH;
    for (int k = 0; k < 9; ++k) sh[k] = shb.c[k];
    vec3 ambient = oxEvalSH9(sh, vec3(0.0, 1.0, 0.0)) * VIEW.iblIntensity;
    vec3 sunLight = vec3(0.0);
    if (VIEW.sunLight >= 0) {
        Light l = SCENE.lights.l[VIEW.sunLight];
        sunLight = l.color * max(-l.direction.y, 0.0) * sunShadow * OX_INV_PI;
    }
    vec3 inscatter = WATER.scatter.rgb * (ambient + sunLight);
    vec3 transmittance = exp(-WATER.absorption.rgb * path);
    if (below) {
        // Looking up through the surface: the air side is not absorbed by the water body.
        transmittance = vec3(1.0);
        inscatter = vec3(0.0);
    }
    vec3 refracted = bottom * transmittance + inscatter * (1.0 - transmittance);

    // --- reflection ---
    vec3 R = reflect(-V, N);
    // Wave normals tilted away from grazing views reflect below the horizon: keep R in the upper hemisphere.
    if (!below && R.y < 0.05) R = normalize(vec3(R.x, 0.05, R.z));
    vec3 reflection = oxSamplePrefiltered(pc.view, R, roughness) * VIEW.iblIntensity;
    if (below) reflection = WATER.underwater.rgb * ambient; // total internal reflection shows the water body
    if (!below) {
        bool planarHit = false;
#ifdef OX_HAS_PLANAR_REFLECTIONS
        OxPlanarReflection pr;
        if (oxFindPlanarReflection(pc.view, P, 0.5 + WATER.detail.w, pr)) {
            vec4 r = oxSamplePlanarReflection(pc.view, pr, uv, N);
            reflection = mix(reflection, r.rgb, r.a);
            planarHit = r.a > 0.0;
        }
#endif
        if (!planarHit) {
            vec4 ssr = traceSsr(P, R, roughness);
            reflection = mix(reflection, ssr.rgb / max(VIEW.preExposure, 1e-12), ssr.a);
        }
    }

    // --- Fresnel (Schlick, F0 = 0.02); from below, total internal reflection beyond the critical angle ---
    float cosV = clamp(dot(N, V), 0.0, 1.0);
    float F;
    if (below) {
        float sinT2 = (1.0 - cosV * cosV) * kWaterIor * kWaterIor;
        F = sinT2 >= 1.0 ? 1.0 : oxSchlick(sqrt(1.0 - sinT2), 0.02);
    } else {
        F = oxSchlick(cosV, 0.02);
    }

    vec3 color = mix(refracted, reflection, F) + specular;

    // --- foam: shoreline (water depth above the opaque scene) and wave crests ---
    if (!below && WATER.foam.z > 0.0) {
        vec3 floorPos = oxWorldPositionFromDepth(pc.view, uv, OX_SAMPLE_2D_LOD(pc.sceneDepth, OX_SAMPLER_NEAREST_CLAMP, uv, 0.0).r);
        float waterDepth = sceneLin >= 1e5 ? 1e3 : max(P.y - floorPos.y, 0.0);
        float shore = 1.0 - clamp(waterDepth / max(WATER.foam.x, 1e-3), 0.0, 1.0);
        float crest = clamp((vCrest - (1.0 - WATER.foam.y)) / max(WATER.foam.y, 1e-3), 0.0, 1.0);
        float noise = 0.5;
        if (WATER.normalMap != OX_INVALID_INDEX) {
            vec2 fuv = vRest * 0.9 + vec2(0.3, 0.2) * WATER.detail.z * time;
            noise = OX_SAMPLE_2D(WATER.normalMap, OX_SAMPLER_ANISO_REPEAT, fuv).a;
        }
        float mask = max(shore * shore, crest);
        float foam = clamp((noise - (1.0 - mask)) * 3.0, 0.0, 1.0) * mask * WATER.foam.z;
        foam = clamp(foam, 0.0, 1.0);
        vec3 foamLight = 0.85 * (ambient + sunLight);
        color = mix(color, foamLight, foam);
    }

    color = oxTranslucencyFog(pc.view, pc.scene, color, P, uv, pc.fog);
    outColor = vec4(color * VIEW.preExposure, 1.0);
}
